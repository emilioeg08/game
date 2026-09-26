// entity.storage: the measured experiment behind ADR-011 (how the World stores entities).
//
// Same kinematic update (semi-implicit Euler step) over N entities, single-threaded so only the memory
// layout differs:
//   dense store    ComponentStore<Kinematics>: sparse set, hot data contiguous (what the World uses)
//   fat structs    std::vector of ~300-byte structs mixing hot and cold data (typical "Ship" class)
//   heap objects   std::vector<std::unique_ptr<Base>> with a virtual update, visited in shuffled order
//                  (long-lived object graphs end up scattered in memory)
//   hash map       std::unordered_map<u64, FatShip> (id -> object, a common first design)
// Plus random lookup by id and churn (destroy + create 10%).

#include "Benchmarks/BenchCommon.h"

#include "Engine/Core/Random.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Profiling/Statistics.h"
#include "Engine/Time/Stopwatch.h"
#include "Simulation/World/World.h"

#include <algorithm>
#include <memory>
#include <numeric>
#include <unordered_map>

namespace gx::bench {
namespace {

constexpr f64 kDt = 1.0;

struct Kinematics {
    Vec3d position;
    Vec3d velocity;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io(position);
        ar.io(velocity);
    }
};

struct FatShip {
    Vec3d position;
    Vec3d velocity;
    char name[64] = {};
    f64 cargo[16] = {};
    u64 flags[8] = {};
};

class ShipObject {
public:
    virtual ~ShipObject() = default;
    virtual void update(f64 dt) = 0;
    Vec3d position;
    Vec3d velocity;
};

class FreighterObject final : public ShipObject {
public:
    void update(f64 dt) override {
        velocity += Vec3d{0.0, 0.0, -1e-3} * dt;
        position += velocity * dt;
    }
    char cold[200] = {};
};

inline void integrate(Vec3d& position, Vec3d& velocity) {
    velocity += Vec3d{0.0, 0.0, -1e-3} * kDt;
    position += velocity * kDt;
}

template <typename Fn>
f64 medianNsPerItem(u32 items, int repetitions, Fn&& fn) {
    std::vector<f64> samples;
    for (int r = 0; r < repetitions; ++r) {
        const Stopwatch timer;
        fn();
        samples.push_back(static_cast<f64>(timer.elapsedNs()) / items);
    }
    return computeSampleStats(samples).p50;
}

} // namespace

void benchEntityStorage(Report& report, const Options& options) {
    section("entity.storage (single thread, ns per entity; lower is better)");
    const std::vector<u32> sizes =
        options.quick ? std::vector<u32>{10'000, 100'000} : std::vector<u32>{10'000, 100'000, 1'000'000};
    std::printf("%10s %10s %10s %10s %10s %12s %12s %12s %12s\n", "entities", "dense", "fat", "heap virt",
                "hash map", "lookup dense", "lookup map", "churn world", "churn map");

    f64 checksum = 0.0;
    for (const u32 n : sizes) {
        const int repetitions = n >= 1'000'000 ? 5 : 11;
        Rng rng(n);

        // Dense store, filled through a World like the simulation does.
        World world;
        ComponentStore<Kinematics>& store = world.registerComponent<Kinematics>("Kinematics");
        std::vector<EntityId> ids;
        ids.reserve(n);
        for (u32 i = 0; i < n; ++i) {
            ids.push_back(world.createEntity());
            store.add(ids.back(), Kinematics{{rng.nextF64(), rng.nextF64(), 0.0}, {1.0, 0.0, 0.0}});
        }
        const f64 dense = medianNsPerItem(n, repetitions, [&] {
            for (Kinematics& k : store.values()) {
                integrate(k.position, k.velocity);
            }
        });

        std::vector<FatShip> fat(n);
        const f64 fatNs = medianNsPerItem(n, repetitions, [&] {
            for (FatShip& ship : fat) {
                integrate(ship.position, ship.velocity);
            }
        });

        std::vector<std::unique_ptr<ShipObject>> objects;
        objects.reserve(n);
        std::vector<std::unique_ptr<char[]>> interleaved; // other allocations in between, as in a real heap
        for (u32 i = 0; i < n; ++i) {
            objects.push_back(std::make_unique<FreighterObject>());
            interleaved.push_back(std::make_unique<char[]>(64 + rng.uniformU32(256)));
        }
        for (u32 i = n - 1; i > 0; --i) {
            std::swap(objects[i], objects[rng.uniformU32(i + 1)]);
        }
        const f64 heapNs = medianNsPerItem(n, repetitions, [&] {
            for (const auto& object : objects) {
                object->update(kDt);
            }
        });

        std::unordered_map<u64, FatShip> map;
        map.reserve(n);
        for (u32 i = 0; i < n; ++i) {
            map.emplace(mix64(i), FatShip{});
        }
        const f64 mapNs = medianNsPerItem(n, repetitions, [&] {
            for (auto& entry : map) {
                integrate(entry.second.position, entry.second.velocity);
            }
        });

        // Random lookups by id.
        std::vector<u32> order(n);
        std::iota(order.begin(), order.end(), 0u);
        for (u32 i = n - 1; i > 0; --i) {
            std::swap(order[i], order[rng.uniformU32(i + 1)]);
        }
        const f64 lookupDense = medianNsPerItem(n, repetitions, [&] {
            for (const u32 i : order) {
                checksum += store.get(ids[i]).position.x;
            }
        });
        const f64 lookupMap = medianNsPerItem(n, repetitions, [&] {
            for (const u32 i : order) {
                checksum += map.find(mix64(i))->second.position.x;
            }
        });

        // Churn: destroy 10% of the entities and create as many (one "destroy + create" per item).
        const u32 churn = n / 10;
        const f64 churnWorld = medianNsPerItem(churn, 3, [&] {
            for (u32 c = 0; c < churn; ++c) {
                const u32 slot = order[c];
                world.destroyEntity(ids[slot]);
                ids[slot] = world.createEntity();
                store.add(ids[slot], Kinematics{});
            }
        });
        u64 nextKey = n;
        const f64 churnMap = medianNsPerItem(churn, 3, [&] {
            for (u32 c = 0; c < churn; ++c) {
                map.erase(map.begin());
                map.emplace(mix64(nextKey++), FatShip{});
            }
        });

        for (const Kinematics& k : store.values()) {
            checksum += k.position.x;
        }
        for (const FatShip& ship : fat) {
            checksum += ship.position.x;
        }
        for (const auto& object : objects) {
            checksum += object->position.x;
        }

        std::printf("%10u %10.2f %10.2f %10.2f %10.2f %12.2f %12.2f %12.2f %12.2f\n", n, dense, fatNs, heapNs,
                    mapNs, lookupDense, lookupMap, churnWorld, churnMap);
        std::fflush(stdout);
        report.add("entity.storage", {{"entities", Report::integer(n)},
                                      {"update_dense_ns", Report::number(dense)},
                                      {"update_fat_struct_ns", Report::number(fatNs)},
                                      {"update_heap_virtual_ns", Report::number(heapNs)},
                                      {"update_hash_map_ns", Report::number(mapNs)},
                                      {"lookup_dense_ns", Report::number(lookupDense)},
                                      {"lookup_hash_map_ns", Report::number(lookupMap)},
                                      {"churn_world_ns", Report::number(churnWorld)},
                                      {"churn_hash_map_ns", Report::number(churnMap)}});
    }
    std::printf("(checksum %.3e)\n", checksum);
}

} // namespace gx::bench
