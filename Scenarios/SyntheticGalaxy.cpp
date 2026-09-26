#include "Scenarios/SyntheticGalaxy.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"
#include "Engine/Jobs/JobSystem.h"
#include "Simulation/Kernel/Simulation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gx {
namespace {

constexpr f64 kSolarGm = 1.32712440018e20;        // m^3/s^2
constexpr f64 kAstronomicalUnit = 1.495978707e11; // m
constexpr u64 kGenerationStream = fnv1a64("synthetic.generation");
constexpr u64 kEconomyStream = fnv1a64("synthetic.economy");
constexpr f64 kDisruptionChance = 0.01; // per good per economy tick
constexpr f64 kPriceElasticity = 0.05;
constexpr f64 kTradeRate = 0.1;
constexpr f64 kTargetStockHours = 24.0; // desired stock: one day of consumption

template <typename T>
usize bytesOf(const std::vector<T>& v) {
    return v.capacity() * sizeof(T);
}

} // namespace

SyntheticGalaxy::SyntheticGalaxy(const SyntheticGalaxyConfig& config) : m_config(config) {
    GX_CHECK(config.starSystems > 0 && config.bodiesPerSystem > 0 && config.goodsPerSystem > 0,
             "synthetic galaxy needs systems, bodies and goods");
    GX_CHECK(config.motionGrain > 0 && config.systemGrain > 0, "grains must be positive");
    const u64 bodies = static_cast<u64>(config.starSystems) * config.bodiesPerSystem;
    const u64 goods = static_cast<u64>(config.starSystems) * config.goodsPerSystem;
    GX_CHECK(bodies <= std::numeric_limits<u32>::max() && goods <= std::numeric_limits<u32>::max(),
             "synthetic galaxy too large");

    m_starGm.resize(config.starSystems);
    m_position.resize(static_cast<usize>(bodies));
    m_velocity.resize(static_cast<usize>(bodies));
    m_stock.resize(static_cast<usize>(goods));
    m_price.resize(static_cast<usize>(goods));
    m_production.resize(static_cast<usize>(goods));
    m_consumption.resize(static_cast<usize>(goods));
    m_tradeOut.resize(static_cast<usize>(goods), 0.0);

    for (u32 s = 0; s < config.starSystems; ++s) {
        // One stream per star system: the result does not depend on generation order, so generation can
        // later run in parallel or lazily (only for systems that get materialized).
        Rng rng = Rng::forStream(config.seed, kGenerationStream, s);
        const f64 gm = kSolarGm * rng.uniform(0.1, 10.0);
        m_starGm[s] = gm;

        for (u32 b = 0; b < config.bodiesPerSystem; ++b) {
            const usize i = static_cast<usize>(s) * config.bodiesPerSystem + b;
            const f64 radius = kAstronomicalUnit * rng.uniform(0.3, 40.0);
            // Random direction in the orbital plane by rejection sampling: sqrt only, no libm trigonometry.
            f64 dx = 0.0;
            f64 dy = 0.0;
            f64 lengthSq = 0.0;
            do {
                dx = rng.uniform(-1.0, 1.0);
                dy = rng.uniform(-1.0, 1.0);
                lengthSq = dx * dx + dy * dy;
            } while (lengthSq < 1e-4 || lengthSq > 1.0);
            const f64 inverseLength = 1.0 / std::sqrt(lengthSq);
            dx *= inverseLength;
            dy *= inverseLength;
            const f64 speed = std::sqrt(gm / radius) * rng.uniform(0.9, 1.1); // near-circular orbits
            m_position[i] = {dx * radius, dy * radius, radius * rng.uniform(-0.02, 0.02)};
            m_velocity[i] = {-dy * speed, dx * speed, 0.0};
        }

        for (u32 g = 0; g < config.goodsPerSystem; ++g) {
            const usize k = static_cast<usize>(s) * config.goodsPerSystem + g;
            m_stock[k] = rng.uniform(100.0, 1000.0);
            m_production[k] = rng.uniform(5.0, 50.0);
            m_consumption[k] = rng.uniform(5.0, 50.0);
            m_price[k] = rng.uniform(1.0, 100.0);
        }
    }
}

void SyntheticGalaxy::registerSystems(Simulation& simulation) {
    simulation.addSystem(
        {"Synthetic.Motion", TickPhase::Simulation, m_config.motionPeriod, {}, [this](const TickContext& c) {
             updateMotion(c);
         }});
    simulation.addSystem({"Synthetic.Economy",
                          TickPhase::Simulation,
                          m_config.economyPeriod,
                          {},
                          [this](const TickContext& c) { updateEconomy(c); }});
    simulation.addSystem({"Synthetic.TradeCompute",
                          TickPhase::Simulation,
                          m_config.economyPeriod,
                          {},
                          [this](const TickContext& c) { computeTrade(c); }});
    simulation.addSystem({"Synthetic.TradeApply",
                          TickPhase::Synchronization,
                          m_config.economyPeriod,
                          {},
                          [this](const TickContext& c) { applyTrade(c); }});
}

void SyntheticGalaxy::updateMotion(const TickContext& context) {
    const f64 dt = context.dt.toSeconds();
    const u32 bodiesPerSystem = m_config.bodiesPerSystem;
    context.jobs.parallelFor(
        static_cast<u32>(m_position.size()), m_config.motionGrain,
        [&](u32 begin, u32 end) {
            // Semi-implicit Euler around the system's star. Each body only touches its own state.
            for (u32 i = begin; i < end; ++i) {
                const f64 gm = m_starGm[i / bodiesPerSystem];
                const Vec3d r = m_position[i];
                const f64 inverseDistance = 1.0 / std::sqrt(dot(r, r));
                const Vec3d acceleration = r * (-gm * inverseDistance * inverseDistance * inverseDistance);
                m_velocity[i] += acceleration * dt;
                m_position[i] += m_velocity[i] * dt;
            }
        },
        "Synthetic.Motion.Chunks");
}

void SyntheticGalaxy::updateEconomy(const TickContext& context) {
    const f64 hours = context.dt.toHours();
    const u32 goods = m_config.goodsPerSystem;
    const u64 streamSeed = hashCombine(m_config.seed, kEconomyStream);
    context.jobs.parallelFor(
        m_config.starSystems, m_config.systemGrain,
        [&](u32 begin, u32 end) {
            for (u32 s = begin; s < end; ++s) {
                // Randomness keyed by (system, run): independent of threads and execution order.
                Rng rng = Rng::forStream(streamSeed, s, context.runIndex);
                for (u32 g = 0; g < goods; ++g) {
                    const usize k = static_cast<usize>(s) * goods + g;
                    const f64 disruption = rng.chance(kDisruptionChance) ? rng.uniform(0.2, 0.6) : 1.0;
                    const f64 produced = m_production[k] * disruption * rng.uniform(0.9, 1.1) * hours;
                    const f64 available = m_stock[k] + produced;
                    const f64 consumed = std::min(available, m_consumption[k] * hours);
                    m_stock[k] = available - consumed;

                    const f64 target = m_consumption[k] * kTargetStockHours;
                    const f64 scarcity = std::clamp((target - m_stock[k]) / target, -1.0, 1.0);
                    m_price[k] = std::clamp(m_price[k] * (1.0 + kPriceElasticity * scarcity), 0.01, 1.0e6);
                }
            }
        },
        "Synthetic.Economy.Chunks");
}

void SyntheticGalaxy::computeTrade(const TickContext& context) {
    const u32 systems = m_config.starSystems;
    const u32 goods = m_config.goodsPerSystem;
    context.jobs.parallelFor(
        systems, m_config.systemGrain,
        [&](u32 begin, u32 end) {
            // Reads any system's stock, writes only this system's outgoing flow (applied in Synchronization).
            for (u32 s = begin; s < end; ++s) {
                const u32 partner = (s + 1) % systems;
                for (u32 g = 0; g < goods; ++g) {
                    const usize k = static_cast<usize>(s) * goods + g;
                    const usize p = static_cast<usize>(partner) * goods + g;
                    const f64 surplus = m_stock[k] - m_consumption[k] * kTargetStockHours;
                    const f64 deficit = m_consumption[p] * kTargetStockHours - m_stock[p];
                    m_tradeOut[k] =
                        (surplus > 0.0 && deficit > 0.0) ? std::min(surplus, deficit) * kTradeRate : 0.0;
                }
            }
        },
        "Synthetic.TradeCompute.Chunks");
}

void SyntheticGalaxy::applyTrade(const TickContext& context) {
    const u32 systems = m_config.starSystems;
    const u32 goods = m_config.goodsPerSystem;
    context.jobs.parallelFor(
        systems, m_config.systemGrain,
        [&](u32 begin, u32 end) {
            for (u32 s = begin; s < end; ++s) {
                const u32 supplier = (s + systems - 1) % systems;
                for (u32 g = 0; g < goods; ++g) {
                    const usize k = static_cast<usize>(s) * goods + g;
                    m_stock[k] += m_tradeOut[static_cast<usize>(supplier) * goods + g] - m_tradeOut[k];
                }
            }
        },
        "Synthetic.TradeApply.Chunks");

    // Galaxy-wide market value: fixed chunks combined in chunk order, identical for any thread count.
    m_galacticOutput = context.jobs.parallelReduce(
        systems, m_config.systemGrain, 0.0,
        [&](u32 begin, u32 end) {
            f64 sum = 0.0;
            for (usize k = static_cast<usize>(begin) * goods; k < static_cast<usize>(end) * goods; ++k) {
                sum += m_price[k] * m_stock[k];
            }
            return sum;
        },
        [](f64 accumulated, f64 partial) { return accumulated + partial; }, "Synthetic.Output.Chunks");
}

u64 SyntheticGalaxy::stateHash() const {
    StateHasher hasher;
    hasher.add(m_config.starSystems);
    hasher.add(m_config.bodiesPerSystem);
    hasher.add(m_config.goodsPerSystem);
    for (const Vec3d& p : m_position) {
        hasher.add(p.x);
        hasher.add(p.y);
        hasher.add(p.z);
    }
    for (const Vec3d& v : m_velocity) {
        hasher.add(v.x);
        hasher.add(v.y);
        hasher.add(v.z);
    }
    for (usize k = 0; k < m_stock.size(); ++k) {
        hasher.add(m_stock[k]);
        hasher.add(m_price[k]);
        hasher.add(m_tradeOut[k]);
    }
    hasher.add(m_galacticOutput);
    return hasher.value();
}

usize SyntheticGalaxy::stateBytes() const {
    return bytesOf(m_starGm) + bytesOf(m_position) + bytesOf(m_velocity) + bytesOf(m_stock) +
           bytesOf(m_price) + bytesOf(m_production) + bytesOf(m_consumption) + bytesOf(m_tradeOut);
}

} // namespace gx
