#include "Space/Destruction/Debris.h"

#include "Space/Orbits/Kepler.h"

#include <algorithm>
#include <cmath>

namespace gx {
namespace {

Vec3d randomDirection(Rng& rng) {
    // Uniform on the sphere: z uniform in [-1, 1], azimuth uniform.
    const f64 z = rng.uniform(-1.0, 1.0);
    const f64 azimuth = rng.uniform(0.0, kTwoPi);
    const f64 r = std::sqrt(std::max(0.0, 1.0 - z * z));
    return {r * std::cos(azimuth), r * std::sin(azimuth), z};
}

} // namespace

std::vector<FragmentDesc> breakUp(const ShipModules& modules, const Vec3d& velocity, bool reactorBreach, Rng& rng,
                                  const BreakUpRules& rules) {
    // Intact modules, largest first (a stable order: size, then position in the design).
    struct Part {
        ModuleType type;
        f64 size;
        usize index;
    };
    std::vector<Part> intact;
    f64 structureScrap = 0.0;
    for (usize i = 0; i < modules.modules.size(); ++i) {
        const ShipModule& module = modules.modules[i];
        if (module.type == ModuleType::Structure) {
            // What is left of the frame: the share of it that was not shot away.
            structureScrap += module.maxHealth * kScrapPerHealth * 0.5;
            continue;
        }
        if (module.health > 0.0) {
            intact.push_back({module.type, module.maxHealth, i});
        }
    }
    std::stable_sort(intact.begin(), intact.end(), [](const Part& a, const Part& b) { return a.size > b.size; });
    const f64 scrapFactor = reactorBreach ? rules.breachScrap : 1.0;

    std::vector<FragmentDesc> pieces;
    FragmentDesc hulk;
    hulk.hulk = true;
    hulk.modules.push_back(ModuleType::Structure);
    hulk.scrap = structureScrap;
    hulk.velocity = velocity;
    pieces.push_back(hulk);
    // The hulk keeps the largest intact module; the others are dealt out to fragments in turn.
    const auto wanted = static_cast<u32>(std::min<usize>(
        rules.maxFragments, (intact.size() + rules.modulesPerFragment - 1) / std::max(rules.modulesPerFragment, 1u)));
    const u32 fragments = intact.size() <= 1 ? 0 : std::max(1u, wanted);
    for (u32 f = 0; f < fragments; ++f) {
        FragmentDesc fragment;
        fragment.velocity = velocity + randomDirection(rng) * rng.uniform(rules.minKick, rules.maxKick);
        fragment.offset = randomDirection(rng) * rng.uniform(100.0, 1'000.0);
        pieces.push_back(fragment);
    }
    for (usize i = 0; i < intact.size(); ++i) {
        FragmentDesc& piece = i == 0 || fragments == 0 ? pieces.front() : pieces[1 + (i - 1) % fragments];
        piece.modules.push_back(intact[i].type);
        piece.scrap += intact[i].size * kScrapPerHealth;
    }
    f64 total = 0.0;
    for (FragmentDesc& piece : pieces) {
        piece.scrap *= scrapFactor;
        total += piece.scrap;
    }
    for (FragmentDesc& piece : pieces) {
        piece.share = total > 0.0 ? piece.scrap / total : 1.0 / static_cast<f64>(pieces.size());
    }
    return pieces;
}

f64 debrisRadius(const DebrisCloud& cloud, SimTime time) {
    const f64 age = std::max(0.0, (time - cloud.created).toSeconds());
    return std::min(kDebrisMaxRadius, kDebrisInitialRadius + kDebrisExpansion * age);
}

f64 debrisDensity(const DebrisCloud& cloud, SimTime time) {
    const f64 age = std::max(0.0, (time - cloud.created).toSeconds());
    if (age >= kDebrisLifetime) {
        return 0.0;
    }
    const f64 spread = kDebrisInitialRadius / debrisRadius(cloud, time);
    return spread * spread * (1.0 - age / kDebrisLifetime);
}

bool debrisExpired(const DebrisCloud& cloud, SimTime time) {
    return (time - cloud.created).toSeconds() >= kDebrisLifetime;
}

f64 chordThroughSphere(const Vec3d& from, const Vec3d& to, const Vec3d& center, f64 radius) {
    const Vec3d path = to - from;
    const f64 length2 = lengthSquared(path);
    if (length2 <= 0.0) {
        return 0.0;
    }
    // Points from + t * path, t in [0, 1], inside the sphere: solve |from + t path - center|^2 = r^2.
    const Vec3d f = from - center;
    const f64 a = length2;
    const f64 b = 2.0 * dot(f, path);
    const f64 c = lengthSquared(f) - radius * radius;
    const f64 discriminant = b * b - 4.0 * a * c;
    if (discriminant <= 0.0) {
        return 0.0;
    }
    const f64 root = std::sqrt(discriminant);
    const f64 t0 = std::clamp((-b - root) / (2.0 * a), 0.0, 1.0);
    const f64 t1 = std::clamp((-b + root) / (2.0 * a), 0.0, 1.0);
    return (t1 - t0) * std::sqrt(length2);
}

u32 poisson(f64 mean, Rng& rng) {
    if (mean <= 0.0) {
        return 0;
    }
    // Inversion: fine for the small means of debris strikes (clamped to keep it bounded).
    mean = std::min(mean, 30.0);
    const f64 u = rng.nextF64();
    f64 p = std::exp(-mean);
    f64 cumulative = p;
    u32 k = 0;
    while (u > cumulative && k < 100) {
        ++k;
        p *= mean / static_cast<f64>(k);
        cumulative += p;
    }
    return k;
}

std::vector<f64> debrisImpacts(const DebrisCloud& cloud, SimTime time, const Vec3d& from, const Vec3d& to,
                               const Vec3d& velocity, Rng& rng) {
    std::vector<f64> damage;
    const f64 speed = length(velocity - cloud.velocity);
    if (speed < kDebrisSafeSpeed) {
        return damage;
    }
    const f64 density = debrisDensity(cloud, time);
    if (density <= 0.0) {
        return damage;
    }
    const Vec3d center = cloud.centerAt(time);
    const f64 chord = chordThroughSphere(from, to, center, debrisRadius(cloud, time));
    if (chord <= 0.0) {
        return damage;
    }
    const u32 hits = poisson(kDebrisHitsPerKm * density * cloud.mass * chord / 1'000.0, rng);
    const f64 factor = std::min(kDebrisMaxDamageFactor, speed / kDebrisDamageSpeed);
    for (u32 i = 0; i < hits; ++i) {
        damage.push_back(kDebrisDamagePerHit * factor * rng.uniform(0.5, 1.5));
    }
    return damage;
}

} // namespace gx
