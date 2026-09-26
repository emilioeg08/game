#include "Tests/TestFramework.h"

#include "Simulation/World/World.h"

#include <vector>

using namespace gx;

namespace {

struct Health {
    f64 hull = 0.0;
    template <typename Archive>
    void io(Archive& ar) {
        ar.io(hull);
    }
};

struct Crew {
    u32 count = 0;
    template <typename Archive>
    void io(Archive& ar) {
        ar.io(count);
    }
};

} // namespace

GX_TEST(World, RegistryReusesFreedSlotsInFifoOrder) {
    EntityRegistry registry;
    const EntityId a = registry.create();
    const EntityId b = registry.create();
    const EntityId c = registry.create();
    GX_EXPECT(a == (EntityId{0, 0}));
    GX_EXPECT(c == (EntityId{2, 0}));
    registry.destroy(b);
    registry.destroy(a);
    GX_EXPECT(registry.create() == (EntityId{1, 1})); // first freed, first reused, next generation
    GX_EXPECT(registry.create() == (EntityId{0, 1}));
    GX_EXPECT(registry.create() == (EntityId{3, 0}));
    GX_EXPECT_EQ(registry.aliveCount(), 4u);
}

GX_TEST(World, StaleIdsAreNotAlive) {
    World world;
    ComponentStore<Health>& health = world.registerComponent<Health>("Health");
    const EntityId first = world.createEntity();
    health.add(first, {100.0});
    world.destroyEntity(first);
    const EntityId reused = world.createEntity();
    GX_EXPECT_EQ(reused.index, first.index);
    GX_EXPECT(!world.isAlive(first));
    GX_EXPECT(world.isAlive(reused));
    GX_EXPECT(!health.contains(first));
    GX_EXPECT(!health.contains(reused));
    GX_EXPECT(!EntityId{}.isValid());

    test::ScopedAssertCapture capture;
    world.destroyEntity(first); // destroying a stale id is a checked error
    GX_EXPECT_EQ(capture.count(), 1);
}

GX_TEST(World, StoresArePackedAndRemovalSwapsLast) {
    World world;
    ComponentStore<Health>& health = world.registerComponent<Health>("Health");
    const EntityId a = world.createEntity();
    const EntityId b = world.createEntity();
    const EntityId c = world.createEntity();
    health.add(a, {1.0});
    health.add(b, {2.0});
    health.add(c, {3.0});
    health.remove(a);
    GX_REQUIRE(health.size() == 2);
    GX_EXPECT(health.entities()[0] == c); // last element moved into the hole
    GX_EXPECT(health.entities()[1] == b);
    GX_EXPECT_EQ(health.get(c).hull, 3.0);
    GX_EXPECT_EQ(health.get(b).hull, 2.0);
    GX_EXPECT(health.tryGet(a) == nullptr);
}

GX_TEST(World, DestroyingAnEntityRemovesAllItsComponents) {
    World world;
    ComponentStore<Health>& health = world.registerComponent<Health>("Health");
    ComponentStore<Crew>& crew = world.registerComponent<Crew>("Crew");
    const EntityId ship = world.createEntity();
    const EntityId other = world.createEntity();
    health.add(ship, {50.0});
    crew.add(ship, {12});
    crew.add(other, {3});
    world.destroyEntity(ship);
    GX_EXPECT_EQ(health.size(), 0u);
    GX_EXPECT_EQ(crew.size(), 1u);
    GX_EXPECT_EQ(crew.get(other).count, 3u);
    GX_EXPECT_EQ(world.entityCount(), 1u);
}

GX_TEST(World, SerializationRoundTripIsExact) {
    World original;
    ComponentStore<Health>& health = original.registerComponent<Health>("Health");
    ComponentStore<Crew>& crew = original.registerComponent<Crew>("Crew");
    std::vector<EntityId> ids;
    for (u32 i = 0; i < 50; ++i) {
        ids.push_back(original.createEntity());
        health.add(ids.back(), {static_cast<f64>(i) * 1.5});
        if (i % 3 == 0) {
            crew.add(ids.back(), {i});
        }
    }
    for (u32 i = 0; i < 50; i += 4) {
        original.destroyEntity(ids[i]); // leaves holes, reordered stores and a free list
    }
    const EntityId late = original.createEntity();
    health.add(late, {-1.0});

    BinaryWriter writer;
    original.write(writer);

    World copy;
    copy.registerComponent<Health>("Health");
    copy.registerComponent<Crew>("Crew");
    BinaryReader reader(writer.bytes());
    copy.read(reader);
    GX_REQUIRE(reader.ok());

    BinaryWriter rewritten;
    copy.write(rewritten);
    GX_EXPECT(rewritten.bytes() == writer.bytes());
    GX_EXPECT_EQ(copy.entityCount(), original.entityCount());
    GX_EXPECT_EQ(copy.components<Health>().get(late).hull, -1.0);
    // Allocation continues identically after loading.
    GX_EXPECT(copy.createEntity() == original.createEntity());
}

GX_TEST(World, ReadRejectsDifferentRegistrations) {
    World original;
    original.registerComponent<Health>("Health");
    original.components<Health>().add(original.createEntity(), {1.0});
    BinaryWriter writer;
    original.write(writer);

    World renamed;
    renamed.registerComponent<Health>("Hull");
    BinaryReader reader(writer.bytes());
    renamed.read(reader);
    GX_EXPECT(!reader.ok());
    GX_EXPECT(reader.error().find("Health") != std::string::npos);

    World extra;
    extra.registerComponent<Health>("Health");
    extra.registerComponent<Crew>("Crew");
    BinaryReader extraReader(writer.bytes());
    extra.read(extraReader);
    GX_EXPECT(!extraReader.ok());
}
