#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/World/EntityRegistry.h"

#include <string>

// Wrecks (ADR-039): what is left of a destroyed ship. Each piece is an entity with Kinematics (it drifts),
// a CargoHold (the cargo that survived and the scrap of its intact modules) and a Wreck. Nobody sees them
// through walls: a faction knows a wreck if it watched the ship die or if one of its ships came close.
namespace gx {

struct Wreck {
    std::string name; // the ship it was
    u32 shipClass = 0;
    u32 faction = 0; // of the ship it was
    SimTime created;
    SimTime expires; // drifts out of reach: what it still holds is lost
    u32 knownBy = 0; // bit per faction
    bool hulk = false;

    [[nodiscard]] bool knownTo(u32 viewer) const { return (knownBy >> viewer & 1u) != 0; }

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("name", name);
        ar.io("shipClass", shipClass);
        ar.io("faction", faction);
        ar.io("created", created);
        ar.io("expires", expires);
        ar.io("knownBy", knownBy);
        ar.io("hulk", hulk);
    }
};

// Player input: take what a wreck next to the ship holds (as much as fits).
struct SalvageCommand {
    EntityId ship;
    EntityId wreck;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("ship", ship);
        ar.io("wreck", wreck);
    }
};

} // namespace gx
