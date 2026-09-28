#include "Space/Bodies/CelestialBody.h"

#include "Engine/Core/Assert.h"
#include "Simulation/World/World.h"

namespace gx {

const char* toString(BodyKind kind) {
    switch (kind) {
    case BodyKind::Star:
        return "Star";
    case BodyKind::RockyPlanet:
        return "RockyPlanet";
    case BodyKind::DesertPlanet:
        return "DesertPlanet";
    case BodyKind::OceanPlanet:
        return "OceanPlanet";
    case BodyKind::IcePlanet:
        return "IcePlanet";
    case BodyKind::GasGiant:
        return "GasGiant";
    case BodyKind::Moon:
        return "Moon";
    case BodyKind::Station:
        return "Station";
    case BodyKind::AsteroidField:
        return "AsteroidField";
    case BodyKind::IceField:
        return "IceField";
    case BodyKind::Count:
        break;
    }
    return "Unknown";
}

bool isPlanet(BodyKind kind) {
    return kind == BodyKind::RockyPlanet || kind == BodyKind::DesertPlanet || kind == BodyKind::OceanPlanet ||
           kind == BodyKind::IcePlanet || kind == BodyKind::GasGiant;
}

bool isAsteroidField(BodyKind kind) {
    return kind == BodyKind::AsteroidField || kind == BodyKind::IceField;
}

OrbitState bodyStateAt(const World& world, EntityId body, SimTime time) {
    const auto& orbits = world.components<OrbitsParent>();
    const auto& bodies = world.components<CelestialBody>();
    OrbitState absolute;
    EntityId current = body;
    for (int depth = 0; orbits.contains(current); ++depth) {
        GX_CHECK(depth < 16, "orbit hierarchy too deep (cycle?) at entity {}", current.index);
        if (depth >= 16) {
            break;
        }
        const OrbitsParent& link = orbits.get(current);
        const f64 parentGm = bodies.get(link.parent).gm;
        const OrbitState relative = orbitStateAt(link.orbit, parentGm, time);
        absolute.position += relative.position;
        absolute.velocity += relative.velocity;
        current = link.parent;
    }
    return absolute;
}

} // namespace gx
