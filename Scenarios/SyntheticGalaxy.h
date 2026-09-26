#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Time/SimTime.h"

#include <vector>

namespace gx {

class Simulation;
struct TickContext;

// Synthetic workload for tests, benchmarks and the headless runner. NOT game content.
//
// It is shaped like the real simulation so the kernel, the job system and determinism can be exercised at
// scale before real systems exist:
//   * many star systems with individual free-flying bodies integrated every motion step (ship-like entities;
//     real planets will move on analytic Kepler rails instead),
//   * per-system aggregate economy with random disruptions drawn from per-(system, run) random streams,
//   * cross-system trade computed in the Simulation phase and applied in the Synchronization phase
//     (the double-buffer pattern parallel systems must use to touch other entities' state),
//   * a galaxy-wide deterministic reduction.
// Everything runs at full resolution on purpose: it is the no-LOD baseline future optimizations are
// measured against. The economy is not balanced (prices of permanently scarce goods climb to the clamp);
// only its cost and determinism matter here.
struct SyntheticGalaxyConfig {
    u64 seed = 1;
    u32 starSystems = 100;
    u32 bodiesPerSystem = 16;
    u32 goodsPerSystem = 8;
    SimDuration motionPeriod = SimDuration::minutes(1);
    SimDuration economyPeriod = SimDuration::hours(1);
    // Chunk sizes for parallelFor. Fixed per configuration (never derived from the thread count), which
    // keeps results identical for any number of threads.
    u32 motionGrain = 4096;
    u32 systemGrain = 256;
};

class SyntheticGalaxy {
public:
    explicit SyntheticGalaxy(const SyntheticGalaxyConfig& config);
    SyntheticGalaxy(const SyntheticGalaxy&) = delete;
    SyntheticGalaxy& operator=(const SyntheticGalaxy&) = delete;

    // The registered systems keep a pointer to this object: it must outlive the simulation's use of them.
    void registerSystems(Simulation& simulation);

    [[nodiscard]] u64 stateHash() const;
    [[nodiscard]] const SyntheticGalaxyConfig& config() const { return m_config; }
    [[nodiscard]] usize bodyCount() const { return m_position.size(); }
    [[nodiscard]] usize stateBytes() const;
    // Total market value of all stock, recomputed every economy tick.
    [[nodiscard]] f64 galacticOutput() const { return m_galacticOutput; }

private:
    void updateMotion(const TickContext& context);
    void updateEconomy(const TickContext& context);
    void computeTrade(const TickContext& context);
    void applyTrade(const TickContext& context);

    SyntheticGalaxyConfig m_config;
    // Per star system.
    std::vector<f64> m_starGm; // gravitational parameter, m^3/s^2
    // Per body, system-local coordinates (m, m/s).
    std::vector<Vec3d> m_position;
    std::vector<Vec3d> m_velocity;
    // Per (system, good).
    std::vector<f64> m_stock;
    std::vector<f64> m_price;
    std::vector<f64> m_production;  // units per hour
    std::vector<f64> m_consumption; // units per hour
    std::vector<f64> m_tradeOut;    // amount shipped to the partner system this economy tick
    f64 m_galacticOutput = 0.0;
};

} // namespace gx
