#include "Space/Generation/StarSystemGenerator.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Random.h"
#include "Simulation/World/World.h"

#include <array>
#include <cmath>

namespace gx {
namespace {

constexpr f64 kGravitationalConstant = 6.67430e-11;
constexpr f64 kSolarGm = 1.32712440018e20;
constexpr f64 kSolarRadius = 6.957e8;
constexpr f64 kEarthRadius = 6.371e6;
constexpr f64 kAstronomicalUnit = 1.495978707e11;

constexpr std::array<const char*, 24> kNameStarts = {"Ar",  "Bel", "Cor", "Dra", "Ei",  "Fen", "Gal", "Hes",
                                                     "Ith", "Kel", "Lys", "Mor", "Nev", "Or",  "Pyr", "Qua",
                                                     "Rho", "Sel", "Tar", "Ul",  "Vey", "Xil", "Yss", "Zan"};
constexpr std::array<const char*, 12> kNameMiddles = {"",   "a",  "e",  "i",  "o",  "u",
                                                      "ae", "io", "an", "or", "en", "il"};
constexpr std::array<const char*, 12> kNameEnds = {"ra",  "nis", "thos", "dar", "mir",  "vel",
                                                   "zar", "lon", "ssa",  "tis", "gorn", "ven"};
constexpr std::array<const char*, 9> kRoman = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX"};
constexpr std::array<const char*, 6> kStationTitles = {"Puerto", "Estación", "Atalaya",
                                                       "Muelle", "Enclave",  "Relé"};

template <typename T, usize N>
const T& pick(Rng& rng, const std::array<T, N>& items) {
    return items[rng.uniformU32(static_cast<u32>(N))];
}

f64 sphereGm(f64 radius, f64 density) {
    return kGravitationalConstant * density * (4.0 / 3.0) * kPi * radius * radius * radius;
}

OrbitalElements randomOrbit(Rng& rng, f64 semiMajorAxis, f64 maxEccentricity, f64 maxInclination) {
    OrbitalElements orbit;
    orbit.semiMajorAxis = semiMajorAxis;
    orbit.eccentricity = rng.uniform(0.0, maxEccentricity);
    orbit.inclination = rng.uniform(0.0, maxInclination);
    orbit.longitudeOfAscendingNode = rng.uniform(0.0, kTwoPi);
    orbit.argumentOfPeriapsis = rng.uniform(0.0, kTwoPi);
    orbit.meanAnomalyAtEpoch = rng.uniform(-kPi, kPi);
    return orbit;
}

struct PlanetRecipe {
    BodyKind kind;
    f64 minRadius; // Earth radii
    f64 maxRadius;
    f64 density; // kg/m^3
};

PlanetRecipe planetRecipe(BodyKind kind) {
    switch (kind) {
    case BodyKind::DesertPlanet:
        return {kind, 0.5, 1.3, 4800.0};
    case BodyKind::OceanPlanet:
        return {kind, 0.8, 1.6, 5000.0};
    case BodyKind::IcePlanet:
        return {kind, 0.6, 3.5, 2000.0};
    case BodyKind::GasGiant:
        return {kind, 3.5, 11.5, 1300.0};
    default:
        return {BodyKind::RockyPlanet, 0.4, 1.5, 5500.0};
    }
}

BodyKind choosePlanetKind(Rng& rng, f64 distanceAu, f64 habitableInner, f64 habitableOuter, f64 snowLine) {
    const f64 roll = rng.nextF64();
    if (distanceAu < habitableInner) {
        return roll < 0.6 ? BodyKind::RockyPlanet : BodyKind::DesertPlanet;
    }
    if (distanceAu <= habitableOuter) {
        return roll < 0.55 ? BodyKind::OceanPlanet
                           : (roll < 0.8 ? BodyKind::RockyPlanet : BodyKind::DesertPlanet);
    }
    if (distanceAu < snowLine) {
        return roll < 0.5 ? BodyKind::DesertPlanet : BodyKind::RockyPlanet;
    }
    return roll < 0.65 ? BodyKind::GasGiant : BodyKind::IcePlanet;
}

u32 moonCount(Rng& rng, BodyKind kind) {
    switch (kind) {
    case BodyKind::GasGiant:
        return 1 + rng.uniformU32(4);
    case BodyKind::IcePlanet:
        return rng.uniformU32(3);
    default:
        return rng.chance(0.3) ? 1u : 0u;
    }
}

} // namespace

std::string generateName(Rng& rng) {
    std::string name = std::string(pick(rng, kNameStarts)) + pick(rng, kNameMiddles) + pick(rng, kNameEnds);
    return name;
}

StarSystemDesc generateStarSystem(u64 seed) {
    Rng rng(hashCombine(seed, fnv1a64("space.star-system")));
    StarSystemDesc system;
    system.seed = seed;
    system.name = generateName(rng);

    const f64 starMass = rng.uniform(0.6, 1.4); // solar masses
    const f64 luminosity = std::pow(starMass, 3.5);
    const f64 habitableInner = 0.95 * std::sqrt(luminosity);
    const f64 habitableOuter = 1.4 * std::sqrt(luminosity);
    const f64 snowLine = 2.7 * std::sqrt(luminosity);

    BodyDesc star;
    star.name = system.name;
    star.kind = BodyKind::Star;
    star.radius = kSolarRadius * std::pow(starMass, 0.8);
    star.gm = kSolarGm * starMass;
    system.bodies.push_back(star);

    const u32 planetCount = 4 + rng.uniformU32(6);
    f64 semiMajorAxisAu = rng.uniform(0.3, 0.5) * std::sqrt(luminosity);
    std::vector<i32> planets;
    for (u32 p = 0; p < planetCount; ++p) {
        const BodyKind kind =
            choosePlanetKind(rng, semiMajorAxisAu, habitableInner, habitableOuter, snowLine);
        const PlanetRecipe recipe = planetRecipe(kind);
        BodyDesc planet;
        planet.name = system.name + " " + kRoman[p];
        planet.kind = kind;
        planet.radius = kEarthRadius * rng.uniform(recipe.minRadius, recipe.maxRadius);
        planet.gm = sphereGm(planet.radius, recipe.density);
        planet.parent = 0;
        planet.orbit = randomOrbit(rng, semiMajorAxisAu * kAstronomicalUnit, 0.07, 0.04);
        const auto planetIndex = static_cast<i32>(system.bodies.size());
        system.bodies.push_back(planet);
        planets.push_back(planetIndex);

        // Moons stay well inside the Hill sphere so the two-body approximation holds.
        const f64 hillRadius = planet.orbit.semiMajorAxis * (1.0 - planet.orbit.eccentricity) *
                               std::cbrt(planet.gm / (3.0 * star.gm));
        f64 moonDistance = planet.radius * rng.uniform(4.0, 12.0);
        const u32 moons = moonCount(rng, kind);
        for (u32 m = 0; m < moons && moonDistance < 0.3 * hillRadius; ++m) {
            BodyDesc moon;
            moon.name = planet.name + " " + static_cast<char>('a' + m);
            moon.kind = BodyKind::Moon;
            moon.radius = kEarthRadius * rng.uniform(0.1, kind == BodyKind::GasGiant ? 0.45 : 0.3);
            moon.gm = sphereGm(moon.radius, 3000.0);
            moon.parent = planetIndex;
            moon.orbit = randomOrbit(rng, moonDistance, 0.03, 0.05);
            system.bodies.push_back(moon);
            moonDistance *= rng.uniform(1.4, 2.2);
        }
        semiMajorAxisAu *= rng.uniform(1.45, 2.0);
    }

    // Stations: the main port around the most habitable planet, a second one around a gas giant if any.
    const auto firstOfKind = [&](BodyKind kind) {
        for (const i32 index : planets) {
            if (system.bodies[static_cast<usize>(index)].kind == kind) {
                return index;
            }
        }
        return -1;
    };
    i32 home = firstOfKind(BodyKind::OceanPlanet);
    if (home < 0) {
        home = firstOfKind(BodyKind::RockyPlanet);
    }
    if (home < 0) {
        home = planets.front();
    }
    i32 second = firstOfKind(BodyKind::GasGiant);
    if (second < 0 || second == home) {
        second = planets.back() != home ? planets.back() : planets.front();
    }
    for (const i32 hostIndex : {home, second}) {
        const BodyDesc& host = system.bodies[static_cast<usize>(hostIndex)];
        BodyDesc station;
        station.name = std::string(pick(rng, kStationTitles)) + " " + host.name;
        station.kind = BodyKind::Station;
        station.radius = 800.0;
        station.gm = 0.0;
        station.parent = hostIndex;
        station.orbit = randomOrbit(rng, host.radius + rng.uniform(400e3, 2000e3), 0.001, 0.1);
        system.bodies.push_back(station);
    }
    return system;
}

std::vector<EntityId> spawnStarSystem(World& world, const StarSystemDesc& desc) {
    auto& bodies = world.components<CelestialBody>();
    auto& orbits = world.components<OrbitsParent>();
    std::vector<EntityId> ids;
    ids.reserve(desc.bodies.size());
    for (const BodyDesc& body : desc.bodies) {
        const EntityId id = world.createEntity();
        bodies.add(id, {body.name, body.kind, body.radius, body.gm});
        if (body.parent >= 0) {
            GX_CHECK(static_cast<usize>(body.parent) < ids.size(), "body '{}' listed before its parent",
                     body.name);
            orbits.add(id, {ids[static_cast<usize>(body.parent)], body.orbit});
        }
        ids.push_back(id);
    }
    return ids;
}

} // namespace gx
