#pragma once

#include "Engine/Core/Random.h"
#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Time/SimTime.h"
#include "Space/Ships/Modules.h"

#include <vector>

// Localized destruction (GDD §10, ADR-039). A destroyed ship does not vanish: it breaks up along its modules.
// Modules still in one piece end up in the hulk or in fragments and can be salvaged as scrap; modules wrecked
// in the fight are vaporised. The explosion throws a cloud of debris that is dangerous to cross fast. Pure
// functions of their inputs, without World or content dependencies: the game decides names, goods and prices.
namespace gx {

// Tonnes of salvageable scrap per point of health of a module that survived.
inline constexpr f64 kScrapPerHealth = 0.01;

// One piece of a destroyed ship: which modules it carries, where it goes, and its share of what survived.
struct FragmentDesc {
    std::vector<ModuleType> modules; // intact modules in this piece (the hulk also has the structure)
    f64 scrap = 0.0;                 // t
    f64 share = 0.0;                 // of the surviving cargo, by size
    Vec3d velocity;                  // absolute, m/s
    Vec3d offset;                    // from the ship's position, m
    bool hulk = false;
};

struct BreakUpRules {
    u32 maxFragments = 4;         // besides the hulk
    u32 modulesPerFragment = 3;   // intact modules a fragment carries at most
    f64 minKick = 50.0;           // m/s: the explosion pushes pieces apart
    f64 maxKick = 400.0;
    f64 breachScrap = 0.5;        // a reactor breach leaves this share of the scrap
};

// Splits a destroyed ship. The hulk keeps the structure (if any part of it is left) and the largest intact
// modules; the rest of the intact modules are dealt to up to maxFragments pieces. Deterministic for a given
// `rng` state.
[[nodiscard]] std::vector<FragmentDesc> breakUp(const ShipModules& modules, const Vec3d& velocity,
                                                bool reactorBreach, Rng& rng, const BreakUpRules& rules = {});

// Share of the cargo that survives an explosion.
[[nodiscard]] inline f64 cargoSurvival(bool reactorBreach) {
    return reactorBreach ? 0.25 : 0.5;
}

// The debris thrown by an explosion: a sphere that expands and thins out as it drifts. A ship crossing it
// suffers impacts in proportion to the distance it travels through it and to its density; below
// kDebrisSafeSpeed relative to the cloud (a salvager matching its drift) it is safe.
struct DebrisCloud {
    Vec3d origin; // at `created`
    Vec3d velocity;
    SimTime created;
    f64 mass = 1.0; // relative size of the ship that made it (1: a Carguero)

    [[nodiscard]] Vec3d centerAt(SimTime time) const { return origin + velocity * (time - created).toSeconds(); }

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("origin", origin);
        ar.io("velocity", velocity);
        ar.io("created", created);
        ar.io("mass", mass);
    }
};

inline constexpr f64 kDebrisInitialRadius = 2'000.0;  // m
inline constexpr f64 kDebrisExpansion = 20.0;         // m/s
inline constexpr f64 kDebrisMaxRadius = 30'000.0;     // m
inline constexpr f64 kDebrisLifetime = 1'800.0;       // s: then too thin to matter
inline constexpr f64 kDebrisSafeSpeed = 1'000.0;      // m/s relative to the cloud
inline constexpr f64 kDebrisHitsPerKm = 0.1;          // expected impacts per km crossed, fresh cloud, mass 1
inline constexpr f64 kDebrisDamagePerHit = 8.0;       // at 10 km/s relative; grows with speed, capped
inline constexpr f64 kDebrisDamageSpeed = 10'000.0;   // m/s
inline constexpr f64 kDebrisMaxDamageFactor = 5.0;

[[nodiscard]] f64 debrisRadius(const DebrisCloud& cloud, SimTime time);
// Relative density: 1 when fresh, fading as the cloud expands (1/r^2 of its growth) and ages.
[[nodiscard]] f64 debrisDensity(const DebrisCloud& cloud, SimTime time);
[[nodiscard]] bool debrisExpired(const DebrisCloud& cloud, SimTime time);

// Length (m) of the straight path from `from` to `to` that lies inside a sphere.
[[nodiscard]] f64 chordThroughSphere(const Vec3d& from, const Vec3d& to, const Vec3d& center, f64 radius);

// Impacts on a ship that moved from `from` to `to` (in the cloud's frame: positions at `time` shifted by the
// cloud's own drift) with velocity `velocity`, through the cloud at `time`: a Poisson draw with mean hitsPerKm x density x mass x km crossed (none below the safe
// relative speed). Returns the damage of each impact.
[[nodiscard]] std::vector<f64> debrisImpacts(const DebrisCloud& cloud, SimTime time, const Vec3d& from,
                                             const Vec3d& to, const Vec3d& velocity, Rng& rng);

// Poisson draw by inversion (small means).
[[nodiscard]] u32 poisson(f64 mean, Rng& rng);

} // namespace gx
