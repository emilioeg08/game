#pragma once

#include "Engine/Core/Types.h"
#include "Simulation/World/EntityRegistry.h"
#include "Space/Orbits/Kepler.h"

#include <string>

namespace gx {

class World;

enum class BodyKind : u8 {
    Star,
    RockyPlanet,
    DesertPlanet,
    OceanPlanet,
    IcePlanet,
    GasGiant,
    Moon,
    Station,
    Count
};

[[nodiscard]] const char* toString(BodyKind kind);
[[nodiscard]] bool isPlanet(BodyKind kind);

// Star, planet, moon or station. Physical data only; what a body means for the economy or politics
// belongs to other components.
struct CelestialBody {
    std::string name;
    BodyKind kind = BodyKind::RockyPlanet;
    f64 radius = 0.0; // m
    f64 gm = 0.0;     // m^3/s^2 (0 for stations)

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("name", name);
        ar.io("kind", kind);
        ar.io("radius", radius);
        ar.io("gm", gm);
    }
};

// Moves on analytic Kepler rails around `parent`. Bodies without it (the star) sit at the system origin.
struct OrbitsParent {
    EntityId parent;
    OrbitalElements orbit;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("parent", parent);
        ar.io("orbit", orbit);
    }
};

// Absolute state in the system frame (origin at the star), walking the parent chain.
[[nodiscard]] OrbitState bodyStateAt(const World& world, EntityId body, SimTime time);

} // namespace gx
