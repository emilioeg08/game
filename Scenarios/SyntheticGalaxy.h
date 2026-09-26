#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/World/EntityRegistry.h"

#include <vector>

namespace gx {

class BinaryReader;
class BinaryWriter;
class Simulation;
class World;
struct TickContext;

// Synthetic workload for tests, benchmarks and the headless runner. NOT game content.
//
// It is shaped like the real simulation so the kernel can be exercised at scale before real systems exist:
//   * many star systems with individual free-flying bodies integrated every motion step (ship-like entities;
//     real planets will move on analytic Kepler rails instead),
//   * per-system aggregate economy with random disruptions drawn from per-(system, run) random streams,
//   * cross-system trade computed in the Simulation phase and applied in the Synchronization phase
//     (the double-buffer pattern parallel systems must use to touch other entities' state),
//   * convoys as World entities: the economy requests them (parallel event emission), event subscribers
//     spawn them, a parallel system moves them, arrival events deliver the cargo and destroy them,
//   * commands as external input (spawn a convoy, raid the convoys bound for a system),
//   * a galaxy-wide deterministic reduction.
// Everything runs at full resolution on purpose: it is the no-LOD baseline future optimizations are
// measured against. The economy is not balanced (prices of permanently scarce goods climb to the clamp);
// only its cost, causality and determinism matter here.
struct SyntheticGalaxyConfig {
    u64 seed = 1; // generation seed; runtime randomness uses the simulation's world seed
    u32 starSystems = 100;
    u32 bodiesPerSystem = 16;
    u32 goodsPerSystem = 8;
    SimDuration motionPeriod = SimDuration::minutes(1);
    SimDuration economyPeriod = SimDuration::hours(1);
    bool convoys = true;
    // Chunk sizes for parallelFor. Fixed per configuration (never derived from the thread count), which
    // keeps results identical for any number of threads. motionGrain chosen with the sim.grain benchmark
    // (docs/BENCHMARKS.md): 4096+ is clearly too coarse at 1,000 systems; 1024 was best or near-best at every
    // scale, while differences between 256 and 2048 were within run-to-run noise.
    u32 motionGrain = 1024;
    u32 systemGrain = 256;
    u32 convoyGrain = 1024;
};

// Components.
struct SyntheticConvoy {
    u32 origin = 0;
    u32 destination = 0;
    u32 good = 0;
    f64 cargo = 0.0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io(origin);
        ar.io(destination);
        ar.io(good);
        ar.io(cargo);
    }
};

struct SyntheticTransit {
    f64 progress = 0.0; // 0 at departure, >= 1 on arrival
    f64 progressPerHour = 0.0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io(progress);
        ar.io(progressPerHour);
    }
};

// Events.
struct ConvoyRequested {
    u32 origin = 0;
    u32 destination = 0;
    u32 good = 0;
    f64 cargo = 0.0; // already taken from the origin's stock
    f64 travelHours = 0.0;
};

struct ConvoyArrived {
    EntityId convoy;
};

// Commands (external input: player, debug console, scripts).
struct SpawnConvoyCommand {
    u32 origin = 0;
    u32 destination = 0;
    u32 good = 0;
    f64 cargo = 0.0;
    f64 travelHours = 0.0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io(origin);
        ar.io(destination);
        ar.io(good);
        ar.io(cargo);
        ar.io(travelHours);
    }
};

// Destroys every convoy bound for `destination`; their cargo is lost.
struct RaidConvoysCommand {
    u32 destination = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io(destination);
    }
};

struct SyntheticGalaxyStats {
    u64 convoysSpawned = 0;
    u64 convoysArrived = 0;
    u64 convoysRaided = 0;
    u64 commandsRejected = 0;
    f64 cargoDelivered = 0.0;
    f64 cargoLost = 0.0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io(convoysSpawned);
        ar.io(convoysArrived);
        ar.io(convoysRaided);
        ar.io(commandsRejected);
        ar.io(cargoDelivered);
        ar.io(cargoLost);
    }
};

class SyntheticGalaxy {
public:
    explicit SyntheticGalaxy(const SyntheticGalaxyConfig& config);
    SyntheticGalaxy(const SyntheticGalaxy&) = delete;
    SyntheticGalaxy& operator=(const SyntheticGalaxy&) = delete;

    // Registers systems, components, events, commands and the galaxy state block. The registrations keep a
    // pointer to this object: it must outlive the simulation's use of them.
    void install(Simulation& simulation);

    // Hash of the galaxy arrays only (generation checks). Simulation::stateHash() covers everything.
    [[nodiscard]] u64 stateHash() const;
    [[nodiscard]] const SyntheticGalaxyConfig& config() const { return m_config; }
    [[nodiscard]] usize bodyCount() const { return m_position.size(); }
    [[nodiscard]] usize stateBytes() const;
    [[nodiscard]] usize activeConvoys() const;
    [[nodiscard]] const SyntheticGalaxyStats& stats() const { return m_stats; }
    // Total market value of all stock, recomputed every economy tick.
    [[nodiscard]] f64 galacticOutput() const { return m_galacticOutput; }
    [[nodiscard]] f64 stock(u32 system, u32 good) const;

private:
    void updateMotion(const TickContext& context);
    void updateEconomy(const TickContext& context);
    void computeTrade(const TickContext& context);
    void applyTrade(const TickContext& context);
    void moveConvoys(const TickContext& context);

    void spawnConvoy(World& world, u32 origin, u32 destination, u32 good, f64 cargo, f64 travelHours);
    void deliverConvoy(World& world, EntityId convoy);
    void onSpawnCommand(const SpawnConvoyCommand& command, const TickContext& context);
    void onRaidCommand(const RaidConvoysCommand& command, const TickContext& context);

    void writeState(BinaryWriter& writer) const;
    void readState(BinaryReader& reader);

    SyntheticGalaxyConfig m_config;
    World* m_world = nullptr;
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
    SyntheticGalaxyStats m_stats;
};

} // namespace gx
