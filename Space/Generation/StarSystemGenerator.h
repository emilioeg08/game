#pragma once

#include "Engine/Core/Types.h"
#include "Simulation/World/EntityRegistry.h"
#include "Space/Bodies/CelestialBody.h"
#include "Space/Orbits/Kepler.h"

#include <string>
#include <vector>

namespace gx {

class Rng;
class World;

struct BodyDesc {
    std::string name;
    BodyKind kind = BodyKind::RockyPlanet;
    f64 radius = 0.0; // m
    f64 gm = 0.0;     // m^3/s^2
    i32 parent = -1;  // index into StarSystemDesc::bodies; -1 for the star
    OrbitalElements orbit;
};

// A generated star system as plain data: pure function of the seed, testable without a World.
struct StarSystemDesc {
    u64 seed = 0;
    std::string name;
    std::vector<BodyDesc> bodies; // bodies[0] is the star; parents always precede their children
};

// Provisional generation rules (docs/DESIGN.md): one star, 4-9 planets with spacing ratios of 1.45-2.0,
// kinds by distance to the habitable zone and the snow line, moons inside a fraction of each planet's Hill
// sphere, and two stations (the main port around the most habitable planet, a second one around a gas giant
// or another planet).
[[nodiscard]] StarSystemDesc generateStarSystem(u64 seed);

// Invented pronounceable name (never taken from real catalogues).
[[nodiscard]] std::string generateName(Rng& rng);

// Creates one entity per body, in description order, and returns their ids.
std::vector<EntityId> spawnStarSystem(World& world, const StarSystemDesc& desc);

} // namespace gx
