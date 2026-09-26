#include "Simulation/World/EntityRegistry.h"

#include "Engine/Core/Assert.h"
#include "Engine/Serialization/Binary.h"

namespace gx {

EntityId EntityRegistry::create() {
    u32 index = 0;
    if (m_freeHead < m_freeList.size()) {
        index = m_freeList[m_freeHead++];
        compactFreeList();
    } else {
        index = static_cast<u32>(m_generation.size());
        GX_CHECK(index != EntityId::kInvalidIndex, "entity slots exhausted");
        m_generation.push_back(0);
        m_alive.push_back(0);
    }
    m_alive[index] = 1;
    ++m_aliveCount;
    return {index, m_generation[index]};
}

void EntityRegistry::destroy(EntityId id) {
    GX_CHECK(isAlive(id), "destroying a dead or invalid entity ({}:{})", id.index, id.generation);
    if (!isAlive(id)) {
        return;
    }
    m_alive[id.index] = 0;
    ++m_generation[id.index];
    m_freeList.push_back(id.index);
    --m_aliveCount;
}

bool EntityRegistry::isAlive(EntityId id) const {
    return id.index < m_generation.size() && m_alive[id.index] != 0 &&
           m_generation[id.index] == id.generation;
}

void EntityRegistry::compactFreeList() {
    if (m_freeHead >= 1024 && m_freeHead * 2 >= m_freeList.size()) {
        m_freeList.erase(m_freeList.begin(), m_freeList.begin() + static_cast<std::ptrdiff_t>(m_freeHead));
        m_freeHead = 0;
    }
}

void EntityRegistry::write(BinaryWriter& writer) const {
    writer.io(m_generation);
    writer.io(m_alive);
    const std::vector<u32> pendingFree(m_freeList.begin() + static_cast<std::ptrdiff_t>(m_freeHead),
                                       m_freeList.end());
    writer.io(pendingFree);
}

void EntityRegistry::read(BinaryReader& reader) {
    std::vector<u32> generation;
    std::vector<u8> alive;
    std::vector<u32> freeList;
    reader.io(generation);
    reader.io(alive);
    reader.io(freeList);
    if (!reader.ok()) {
        return;
    }
    if (generation.size() != alive.size()) {
        reader.fail("entity registry: slot arrays differ in length");
        return;
    }
    u32 aliveCount = 0;
    std::vector<u8> listedFree(alive.size(), 0);
    for (const u8 flag : alive) {
        if (flag > 1) {
            reader.fail("entity registry: invalid alive flag");
            return;
        }
        aliveCount += flag;
    }
    for (const u32 slot : freeList) {
        if (slot >= alive.size() || alive[slot] != 0 || listedFree[slot] != 0) {
            reader.fail("entity registry: invalid free list");
            return;
        }
        listedFree[slot] = 1;
    }
    if (freeList.size() != alive.size() - aliveCount) {
        reader.fail("entity registry: free list does not cover every dead slot");
        return;
    }
    m_generation = std::move(generation);
    m_alive = std::move(alive);
    m_freeList = std::move(freeList);
    m_freeHead = 0;
    m_aliveCount = aliveCount;
}

} // namespace gx
