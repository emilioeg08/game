#include "Simulation/World/World.h"

#include <format>
#include <unordered_set>

namespace gx {
namespace {
constexpr u32 kStoreChunk = fourCC("CMPS");
constexpr u32 kStoreChunkVersion = 1;
} // namespace

EntityId World::createEntity() {
    GX_ASSERT(JobSystem::currentThreadIndex() == 0, "entities must be created on the main thread");
    return m_registry.create();
}

void World::destroyEntity(EntityId entity) {
    GX_ASSERT(JobSystem::currentThreadIndex() == 0, "entities must be destroyed on the main thread");
    GX_CHECK(m_registry.isAlive(entity), "destroying a dead entity ({}:{})", entity.index, entity.generation);
    if (!m_registry.isAlive(entity)) {
        return;
    }
    for (const auto& store : m_stores) {
        store->removeIfPresent(entity);
    }
    m_registry.destroy(entity);
}

ComponentStoreBase* World::findStore(std::string_view name) const {
    for (const auto& store : m_stores) {
        if (store->name() == name) {
            return store.get();
        }
    }
    return nullptr;
}

void World::write(BinaryWriter& writer) const {
    m_registry.write(writer);
    writer.writeU32(static_cast<u32>(m_stores.size()));
    for (const auto& store : m_stores) {
        const auto mark = writer.beginChunk(kStoreChunk, kStoreChunkVersion);
        writer.writeString(store->name());
        store->write(writer);
        writer.endChunk(mark);
    }
}

void World::read(BinaryReader& reader) {
    m_registry.read(reader);
    const u32 storeCount = reader.readU32();
    if (reader.ok() && storeCount != m_stores.size()) {
        reader.fail(
            std::format("save has {} component types, this build registers {}", storeCount, m_stores.size()));
    }
    std::unordered_set<std::string> seen;
    for (u32 i = 0; i < storeCount && reader.ok(); ++i) {
        BinaryReader::Chunk chunk;
        if (!reader.beginChunk(kStoreChunk, chunk)) {
            break;
        }
        const std::string name = reader.readString();
        ComponentStoreBase* store = findStore(name);
        if (store == nullptr || !seen.insert(name).second) {
            reader.fail(std::format("save has unknown or repeated component type '{}'", name));
            break;
        }
        store->read(reader);
        reader.endChunk(chunk);
    }
    if (!reader.ok()) {
        return;
    }
    for (const auto& store : m_stores) {
        for (const EntityId entity : store->entities()) {
            if (!m_registry.isAlive(entity)) {
                reader.fail(std::format("component '{}' belongs to a dead entity", store->name()));
                return;
            }
        }
    }
}

} // namespace gx
