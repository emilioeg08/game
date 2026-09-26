#pragma once

#include "Engine/Core/Types.h"

#include <compare>
#include <vector>

namespace gx {

class BinaryReader;
class BinaryWriter;

// Stable identity of a simulation entity: a slot index plus the slot's generation. Destroying an entity
// bumps its slot's generation, so stale ids are detected instead of silently aliasing a new entity.
struct EntityId {
    static constexpr u32 kInvalidIndex = 0xffffffffu;

    u32 index = kInvalidIndex;
    u32 generation = 0;

    [[nodiscard]] constexpr bool isValid() const { return index != kInvalidIndex; }
    constexpr bool operator==(const EntityId&) const = default;
    constexpr auto operator<=>(const EntityId&) const = default;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io(index);
        ar.io(generation);
    }
};

// Allocates entity ids. Freed slots are reused first-in first-out, which delays reuse and keeps stale-id
// detection effective. Allocation order depends only on the sequence of create/destroy calls, so it is
// deterministic.
class EntityRegistry {
public:
    [[nodiscard]] EntityId create();
    void destroy(EntityId id);
    [[nodiscard]] bool isAlive(EntityId id) const;

    [[nodiscard]] u32 aliveCount() const { return m_aliveCount; }
    [[nodiscard]] u32 slotCount() const { return static_cast<u32>(m_generation.size()); }

    void write(BinaryWriter& writer) const;
    // Replaces the current contents. Structural errors are reported through the reader.
    void read(BinaryReader& reader);

private:
    void compactFreeList();

    std::vector<u32> m_generation; // current generation of each slot
    std::vector<u8> m_alive;       // 1 if the slot holds a live entity
    std::vector<u32> m_freeList;   // FIFO of free slots, starting at m_freeHead
    usize m_freeHead = 0;
    u32 m_aliveCount = 0;
};

} // namespace gx
