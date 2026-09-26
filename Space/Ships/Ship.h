#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Simulation/World/EntityRegistry.h"

#include <string>

namespace gx {

// Position and velocity in the system frame (m, m/s), as of the last flight step.
struct Kinematics {
    Vec3d position;
    Vec3d velocity;
    Vec3d acceleration; // applied during the last flight step (for display and sensors later)

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("position", position);
        ar.io("velocity", velocity);
        ar.io("acceleration", acceleration);
    }
};

// Provisional flight model (docs/DESIGN.md): Newtonian inertia plus a high-thrust sublight drive. The cruise
// speed is an autopilot limit, not a physical one.
struct ShipDrive {
    f64 maxAcceleration = 0.0; // m/s^2
    f64 cruiseSpeed = 0.0;     // m/s

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("maxAcceleration", maxAcceleration);
        ar.io("cruiseSpeed", cruiseSpeed);
    }
};

enum class FlightMode : u8 {
    Coast,    // no thrust
    Stop,     // kill velocity
    MoveTo,   // fly to a fixed point and stop there
    Approach, // fly to an entity and keep station at `standoff` from it
    Manual,   // thrust along `manualThrust` (player piloting)
    Count
};

[[nodiscard]] const char* toString(FlightMode mode);

struct ShipControl {
    FlightMode mode = FlightMode::Coast;
    EntityId target;      // Approach
    Vec3d point;          // MoveTo
    Vec3d manualThrust;   // Manual: direction scaled by throttle, length <= 1
    f64 standoff = 0.0;   // Approach: keep this distance (m) from the target's centre
    bool arrived = false; // MoveTo/Approach: within tolerance and matched velocity

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("mode", mode);
        ar.io("target", target);
        ar.io("point", point);
        ar.io("manualThrust", manualThrust);
        ar.io("standoff", standoff);
        ar.io("arrived", arrived);
    }
};

struct ShipIdentity {
    std::string name;
    u32 faction = 0;
    u32 shipClass = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("name", name);
        ar.io("faction", faction);
        ar.io("shipClass", shipClass);
    }
};

// Event: a ship reached its MoveTo point or its Approach target.
struct ShipArrived {
    EntityId ship;
    EntityId target; // invalid for MoveTo
};

} // namespace gx
