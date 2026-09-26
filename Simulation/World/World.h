#pragma once

#include "Engine/Core/Assert.h"
#include "Simulation/World/ComponentStore.h"
#include "Simulation/World/EntityRegistry.h"

#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gx {

// Entities and their components. Component types are registered explicitly with a stable name, which is
// what save files refer to; registration order is the serialization order.
//
// No generic query/system machinery on purpose (docs/DECISIONS.md, ADR-011): systems iterate the store they
// drive and look up the others by EntityId.
class World {
public:
    World() = default;
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    template <typename T>
    ComponentStore<T>& registerComponent(std::string_view name) {
        GX_CHECK(!m_storesByType.contains(typeKey<T>()), "component '{}' registered twice", name);
        GX_CHECK(findStore(name) == nullptr, "component name '{}' already in use", name);
        auto store = std::make_unique<ComponentStore<T>>(std::string(name));
        ComponentStore<T>& result = *store;
        m_storesByType.emplace(typeKey<T>(), store.get());
        m_stores.push_back(std::move(store));
        return result;
    }

    template <typename T>
    [[nodiscard]] ComponentStore<T>& components() {
        const auto it = m_storesByType.find(typeKey<T>());
        GX_CHECK(it != m_storesByType.end(), "component type not registered");
        return *static_cast<ComponentStore<T>*>(it->second);
    }
    template <typename T>
    [[nodiscard]] const ComponentStore<T>& components() const {
        return const_cast<World*>(this)->components<T>();
    }

    // Structural changes are main-thread only (Commands and EventResolution phases, or between steps).
    [[nodiscard]] EntityId createEntity();
    // Removes the entity from every store, then frees its id.
    void destroyEntity(EntityId entity);
    [[nodiscard]] bool isAlive(EntityId entity) const { return m_registry.isAlive(entity); }
    [[nodiscard]] u32 entityCount() const { return m_registry.aliveCount(); }
    [[nodiscard]] const EntityRegistry& registry() const { return m_registry; }
    [[nodiscard]] usize componentTypeCount() const { return m_stores.size(); }

    void write(BinaryWriter& writer) const;
    // Requires the same component registrations as the writer; any mismatch fails the reader.
    void read(BinaryReader& reader);

    // Every component of the entity, in registration order (entity inspector).
    void inspect(EntityId entity, FieldVisitor& visitor) const;

private:
    template <typename T>
    static const void* typeKey() {
        static const char key = 0;
        return &key;
    }
    [[nodiscard]] ComponentStoreBase* findStore(std::string_view name) const;

    EntityRegistry m_registry;
    std::vector<std::unique_ptr<ComponentStoreBase>> m_stores; // registration order
    std::unordered_map<const void*, ComponentStoreBase*> m_storesByType;
};

} // namespace gx
