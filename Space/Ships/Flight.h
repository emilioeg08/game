#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/Kernel/SystemScheduler.h"
#include "Simulation/World/EntityRegistry.h"
#include "Space/Orbits/Kepler.h"
#include "Space/Ships/Ship.h"

#include <span>
#include <vector>

namespace gx {

class Simulation;
class World;
struct CelestialBody;
struct TickContext;

// Registers the space component types (bodies and ships) and the space events. Call once per simulation,
// before anything that uses them (the registration order is part of the save structure).
void registerSpaceTypes(Simulation& simulation);

// Hyperspace rules (docs/DESIGN.md, ADR-022).
inline constexpr f64 kMinHyperspaceLeg = 1e9;       // m: shorter trips are flown sublight
inline constexpr f64 kHyperspaceExitDistance = 1e8; // m: drop-out distance from targets outside any well

// Radius of the body's gravity well, where hyperspace cannot be entered and must be left. 0 for bodies that
// have none of their own (moons and stations sit inside their planet's well or are too small to matter).
[[nodiscard]] f64 gravityWellRadius(const CelestialBody& body);

struct GravityWell {
    EntityId body;
    Vec3d position;
    f64 radius = 0.0;
};

struct TargetState {
    Vec3d position;
    Vec3d velocity;
    bool valid = false;
    i32 hostWell = -1; // innermost gravity well containing the target (index into the wells span), or -1
};

// Where a MoveTo/Approach ship is heading at `time`. Invalid if the target no longer exists.
[[nodiscard]] TargetState resolveTarget(const World& world, const ShipControl& control, SimTime time);

// Distance to keep from an Approach target: clear of a planet's surface, close to a station or a ship.
[[nodiscard]] f64 standoffDistance(const World& world, EntityId target);

[[nodiscard]] bool insideGravityWell(const Vec3d& position, std::span<const GravityWell> wells);
[[nodiscard]] i32 innermostWell(const Vec3d& position, std::span<const GravityWell> wells);

// Sublight autopilot: acceleration to apply this step. Updates control.arrived / control.mode and reports the
// transition to "arrived" through `arrivedNow`. Never overshoots: the closing speed is limited both by the
// braking distance and by what one step of length dt can cover.
[[nodiscard]] Vec3d steer(ShipControl& control, const Kinematics& kinematics, const ShipDrive& drive,
                          const TargetState& target, f64 dt, bool& arrivedNow);

struct FlightStepResult {
    bool arrived = false;
    bool enteredHyperspace = false;
    bool leftHyperspace = false;
};

// One flight step of one ship: drive phase transitions (charge, jump, drop-out), sublight steering and
// integration (semi-implicit Euler), or a straight hyperspace leg that stops at the edge of the target's
// gravity well. Pure function of its inputs.
FlightStepResult flyShip(ShipControl& control, Kinematics& kinematics, const ShipDrive& drive,
                         const TargetState& target, std::span<const GravityWell> wells, f64 dt);

// The flight system: flies every ship with a ShipControl. Two passes: gravity wells and targets are resolved
// read-only before any ship moves (ships may target ships), at the time the ships' state refers to; then each
// ship updates only its own state in parallel. Emits ShipArrived and HyperspaceTransition events.
class FlightSystem {
public:
    // grain 256: measured (sim.sandbox) that splitting ~200 ships (~40 ns each) costs more than it saves;
    // parallelism starts to pay with thousands of ships.
    explicit FlightSystem(u32 grain = 256) : m_grain(grain) {}

    // Registers the system with the given period and returns its id (the period is a LOD knob).
    SystemId install(Simulation& simulation, SimDuration period);
    void update(const TickContext& context);

private:
    // Absolute state of a body at the current step's state time, computed on first use and memoized for
    // the step: many ships share a few destinations, so each orbit is solved at most once per step.
    const OrbitState& bodyState(const World& world, EntityId body, SimTime time, int depth = 0);
    TargetState resolveCached(const World& world, const ShipControl& control, SimTime time);

    u32 m_grain;
    std::vector<TargetState> m_targets; // reused between steps
    std::vector<GravityWell> m_wells;
    std::vector<OrbitState> m_bodyStates;
    std::vector<u8> m_bodyStateReady;
};

} // namespace gx
