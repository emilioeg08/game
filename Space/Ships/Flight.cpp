#include "Space/Ships/Flight.h"

#include "Engine/Jobs/JobSystem.h"
#include "Engine/Profiling/Profiler.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Bodies/CelestialBody.h"
#include "Space/Sensors/Sensors.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gx {
namespace {

constexpr f64 kBrakingMargin = 0.8;         // plan braking at 80% of the drive's acceleration
constexpr f64 kArrivalSpeedTolerance = 5.0; // m/s relative to the target
constexpr f64 kMinArrivalTolerance = 100.0; // m
constexpr f64 kStarWellFactor = 20.0;       // gravity well radius, in body radii
constexpr f64 kPlanetWellFactor = 25.0;

Vec3d clampLength(const Vec3d& v, f64 maxLength) {
    const f64 lengthSq = lengthSquared(v);
    if (lengthSq <= maxLength * maxLength) {
        return v;
    }
    return v * (maxLength / std::sqrt(lengthSq));
}

// Distance along a unit ray from `origin` to the surface of the well (0 if already inside, +inf if missed).
f64 distanceToWell(const Vec3d& origin, const Vec3d& direction, const GravityWell& well) {
    const Vec3d fromCenter = origin - well.position;
    const f64 c = lengthSquared(fromCenter) - well.radius * well.radius;
    if (c <= 0.0) {
        return 0.0;
    }
    const f64 b = dot(fromCenter, direction);
    const f64 discriminant = b * b - c;
    if (b >= 0.0 || discriminant < 0.0) {
        return std::numeric_limits<f64>::infinity();
    }
    return -b - std::sqrt(discriminant);
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
    case FlightMode::Pursue:
        return "Pursue";
    case FlightMode::Count:
        break;
    }
    return "Unknown";
}

const char* toString(DrivePhase phase) {
    switch (phase) {
    case DrivePhase::Sublight:
        return "Sublight";
    case DrivePhase::Charging:
        return "Charging";
    case DrivePhase::Hyperspace:
        return "Hyperspace";
    case DrivePhase::Count:
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
    simulation.events().registerEvent<HyperspaceTransition>("Space.HyperspaceTransition");
}

f64 gravityWellRadius(const CelestialBody& body) {
    if (body.kind == BodyKind::Star) {
        return body.radius * kStarWellFactor;
    }
    return isPlanet(body.kind) ? body.radius * kPlanetWellFactor : 0.0;
}

bool insideGravityWell(const Vec3d& position, std::span<const GravityWell> wells) {
    return std::any_of(wells.begin(), wells.end(), [&](const GravityWell& well) {
        return lengthSquared(position - well.position) < well.radius * well.radius;
    });
}

i32 innermostWell(const Vec3d& position, std::span<const GravityWell> wells) {
    i32 best = -1;
    for (usize i = 0; i < wells.size(); ++i) {
        const GravityWell& well = wells[i];
        if (lengthSquared(position - well.position) < well.radius * well.radius &&
            (best < 0 || well.radius < wells[static_cast<usize>(best)].radius)) {
            best = static_cast<i32>(i);
        }
    }
    return best;
}

TargetState resolveTarget(const World& world, const ShipControl& control, SimTime time) {
    if (control.mode == FlightMode::MoveTo) {
        return {control.point, Vec3d{}, true, -1};
    }
    if (control.mode != FlightMode::Approach || !world.isAlive(control.target)) {
        return {};
    }
    if (const Kinematics* ship = world.components<Kinematics>().tryGet(control.target)) {
        return {ship->position, ship->velocity, true, -1};
    }
    if (world.components<CelestialBody>().contains(control.target)) {
        const OrbitState state = bodyStateAt(world, control.target, time);
        return {state.position, state.velocity, true, -1};
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
    case FlightMode::Pursue:
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
    const f64 standoff = control.mode == FlightMode::MoveTo ? 0.0 : control.standoff;
    const f64 gap = distance - standoff; // negative: inside the standoff sphere, back off
    const Vec3d direction = distance > 1e-6 ? offset / distance : Vec3d{};

    const f64 brakingSpeed = std::sqrt(2.0 * kBrakingMargin * maxAcceleration * std::abs(gap));
    const f64 closingSpeed = std::min({drive.cruiseSpeed, brakingSpeed, std::abs(gap) / dt});
    const Vec3d desiredRelativeVelocity = direction * (gap >= 0.0 ? closingSpeed : -closingSpeed);
    const Vec3d acceleration =
        clampLength((desiredRelativeVelocity - relativeVelocity) / dt, maxAcceleration);

    const f64 tolerance = std::max(kMinArrivalTolerance, 0.01 * standoff);
    if (!control.arrived && control.mode != FlightMode::Pursue && std::abs(gap) < tolerance &&
        length(relativeVelocity) < kArrivalSpeedTolerance) {
        control.arrived = true;
        arrivedNow = true;
    }
    return acceleration;
}

FlightStepResult flyShip(ShipControl& control, Kinematics& kinematics, const ShipDrive& drive,
                         const TargetState& target, std::span<const GravityWell> wells, f64 dt) {
    FlightStepResult result;
    const bool autopilot = control.mode == FlightMode::MoveTo || control.mode == FlightMode::Approach ||
                           control.mode == FlightMode::Pursue;

    // New orders that are not autopilot trips (stop, manual, drift) cancel any hyperspace activity, and so
    // does losing the hyperspace drive (damage while charging): a jump at zero speed would never end.
    if (control.phase != DrivePhase::Sublight &&
        (!autopilot || !target.valid || drive.hyperspaceSpeed <= 0.0)) {
        if (control.phase == DrivePhase::Hyperspace) {
            kinematics.velocity = {}; // emergency drop-out: the drive dumps the jump velocity
            result.leftHyperspace = true;
        }
        control.phase = DrivePhase::Sublight;
        control.chargeRemaining = 0.0;
    }

    if (control.phase == DrivePhase::Hyperspace) {
        // Straight leg towards the target, ending at the edge of its gravity well (or at the exit distance).
        const Vec3d offset = target.position - kinematics.position;
        const f64 distance = length(offset);
        const Vec3d direction = distance > 1e-6 ? offset / distance : Vec3d{};
        f64 stopAt = std::max(0.0, distance - kHyperspaceExitDistance);
        if (target.hostWell >= 0) {
            stopAt = std::min(stopAt, distanceToWell(kinematics.position, direction,
                                                     wells[static_cast<usize>(target.hostWell)]));
        }
        const f64 travel = drive.hyperspaceSpeed * dt;
        kinematics.acceleration = {};
        if (travel >= stopAt) {
            kinematics.position += direction * stopAt;
            kinematics.velocity = target.velocity; // drop out matching the destination's frame
            control.phase = DrivePhase::Sublight;
            result.leftHyperspace = true;
        } else {
            kinematics.position += direction * travel;
            kinematics.velocity = direction * drive.hyperspaceSpeed;
        }
        return result;
    }

    const bool inWell = insideGravityWell(kinematics.position, wells);
    if (control.phase == DrivePhase::Charging) {
        if (inWell) {
            control.phase = DrivePhase::Sublight; // cannot jump inside a gravity well
            control.chargeRemaining = 0.0;
        } else {
            control.chargeRemaining -= dt;
            if (control.chargeRemaining <= 0.0) {
                control.chargeRemaining = 0.0;
                control.phase = DrivePhase::Hyperspace; // the leg starts next step
                result.enteredHyperspace = true;
            }
        }
    } else if (autopilot && target.valid && !control.arrived && drive.hyperspaceSpeed > 0.0 && !inWell) {
        // Worth a jump only if the hyperspace leg (up to the edge of the target's well) is long.
        f64 exitDistance = kHyperspaceExitDistance;
        if (target.hostWell >= 0) {
            const GravityWell& well = wells[static_cast<usize>(target.hostWell)];
            exitDistance = well.radius + length(target.position - well.position);
        }
        if (length(target.position - kinematics.position) - exitDistance > kMinHyperspaceLeg) {
            control.phase = DrivePhase::Charging;
            control.chargeRemaining = drive.hyperspaceChargeTime;
        }
    }

    // Sublight: steer and integrate (also while charging: the ship keeps heading to its target).
    bool arrivedNow = false;
    const Vec3d acceleration = steer(control, kinematics, drive, target, dt, arrivedNow);
    kinematics.acceleration = acceleration;
    kinematics.velocity += acceleration * dt;
    kinematics.position += kinematics.velocity * dt;
    result.arrived = arrivedNow;
    return result;
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

TargetState FlightSystem::resolveCached(const World& world, EntityId ship, const ShipControl& control,
                                        SimTime time) {
    TargetState target;
    if (control.mode == FlightMode::Pursue) {
        // The track as the faction knows it, extrapolated from the last detection at constant velocity.
        const ShipIdentity* identity = world.components<ShipIdentity>().tryGet(ship);
        const SensorContact* contact = m_sensors != nullptr && identity != nullptr
                                           ? m_sensors->findContact(identity->faction, control.track)
                                           : nullptr;
        if (contact != nullptr) {
            const f64 age = (time - contact->lastSeen).toSeconds();
            target = {contact->position + contact->velocity * age, contact->velocity, true, -1};
        }
    } else if (control.mode == FlightMode::Approach && world.isAlive(control.target) &&
               world.components<CelestialBody>().contains(control.target)) {
        const OrbitState& state = bodyState(world, control.target, time);
        target = {state.position, state.velocity, true, -1};
    } else {
        target = resolveTarget(world, control, time); // points and ships: no orbit to solve
    }
    if (target.valid) {
        target.hostWell = innermostWell(target.position, m_wells);
    }
    return target;
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

    // Pass 1, read-only: gravity wells and targets, at the instant the ships' state refers to (the previous
    // flight step). Comparing a ship at t - dt with a target at t would bias station keeping by one step of
    // the target's motion (tens of km for a planet). Serial: with body states memoized per step it is a few
    // lookups per ship.
    const SimTime stateTime = context.now - context.dt;
    {
        GX_PROFILE_SCOPE("Space.Flight.Targets");
        const ComponentStore<CelestialBody>& bodies = world.components<CelestialBody>();
        m_bodyStates.resize(bodies.size());
        m_bodyStateReady.assign(bodies.size(), 0);
        m_wells.clear();
        for (usize i = 0; i < bodies.size(); ++i) {
            const f64 radius = gravityWellRadius(bodies.values()[i]);
            if (radius > 0.0) {
                const EntityId body = bodies.entities()[i];
                m_wells.push_back({body, bodyState(world, body, stateTime).position, radius});
            }
        }
        m_targets.resize(count);
        for (u32 i = 0; i < count; ++i) {
            m_targets[i] = resolveCached(world, ships[i], control[i], stateTime);
        }
    }

    // Pass 2: each ship flies and updates only its own state.
    EventChannel<ShipArrived>& arrivals = context.events.channel<ShipArrived>();
    EventChannel<HyperspaceTransition>& transitions = context.events.channel<HyperspaceTransition>();
    const u32 chunks = JobSystem::chunkCount(count, m_grain);
    arrivals.beginParallel(chunks);
    transitions.beginParallel(chunks);
    const std::span<const GravityWell> wells = m_wells;
    context.jobs.parallelFor(
        count, m_grain,
        [&](u32 begin, u32 end) {
            const u32 chunk = begin / m_grain;
            for (u32 i = begin; i < end; ++i) {
                Kinematics& state = kinematics.get(ships[i]);
                const FlightStepResult result =
                    flyShip(control[i], state, drives.get(ships[i]), m_targets[i], wells, dt);
                if (result.arrived) {
                    const EntityId target =
                        control[i].mode == FlightMode::Approach ? control[i].target : EntityId{};
                    arrivals.emitFromChunk(chunk, ShipArrived{ships[i], target});
                }
                if (result.enteredHyperspace || result.leftHyperspace) {
                    transitions.emitFromChunk(
                        chunk, HyperspaceTransition{ships[i], result.enteredHyperspace, state.position});
                }
            }
        },
        "Space.Flight.Ships");
    arrivals.endParallel();
    transitions.endParallel();
}

} // namespace gx
