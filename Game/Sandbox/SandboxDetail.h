#pragma once

#include "Simulation/World/EntityRegistry.h"
#include "Space/Ships/Ship.h"

// Small helpers shared by the Sandbox's translation units. Not part of its interface.
namespace gx::sandbox_detail {

// Key of an entity for per-entity random streams.
inline u64 entityKey(EntityId entity) {
    return (static_cast<u64>(entity.generation) << 32) | entity.index;
}

// New orders keep the drive's state: a ship in hyperspace finishes or aborts its jump through flyShip.
inline void resetOrders(ShipControl& control, FlightMode mode) {
    const DrivePhase phase = control.phase;
    const f64 charge = control.chargeRemaining;
    control = {};
    control.mode = mode;
    control.phase = phase;
    control.chargeRemaining = charge;
}

} // namespace gx::sandbox_detail
