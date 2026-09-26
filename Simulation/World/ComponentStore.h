#pragma once

#include "Engine/Core/Assert.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/Serialization/Binary.h"
#include "Simulation/World/EntityRegistry.h"
#include "Simulation/World/Inspect.h"

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace gx {

class ComponentStoreBase {
public:
    explicit ComponentStoreBase(std::string name) : m_name(std::move(name)) {}
    virtual ~ComponentStoreBase() = default;
    ComponentStoreBase(const ComponentStoreBase&) = delete;
    ComponentStoreBase& operator=(const ComponentStoreBase&) = delete;

    [[nodiscard]] const std::string& name() const { return m_name; }

    [[nodiscard]] virtual bool contains(EntityId entity) const = 0;
    virtual void removeIfPresent(EntityId entity) = 0;
    [[nodiscard]] virtual usize size() const = 0;
    [[nodiscard]] virtual std::span<const EntityId> entities() const = 0;
    virtual void clear() = 0;
    virtual void write(BinaryWriter& writer) const = 0;
    virtual void read(BinaryReader& reader) = 0;
    // Describes the entity's component to a debug tool. Requires contains(entity).
    virtual void inspect(EntityId entity, FieldVisitor& visitor) const = 0;

private:
    std::string m_name;
};

// Components of one type, packed densely (sparse set). Iterating values() is a linear walk over
// contiguous memory, suitable for parallelFor; lookup by EntityId is O(1).
//
// Removal swaps the last element into the hole, so dense order changes, but only as a function of the
// sequence of operations, which keeps iteration order deterministic.
//
// Structural changes (add/remove) are main-thread only; parallel systems may modify values in place.
template <typename T>
class ComponentStore final : public ComponentStoreBase {
public:
    using ComponentStoreBase::ComponentStoreBase;

    T& add(EntityId entity, T value = {}) {
        GX_ASSERT(JobSystem::currentThreadIndex() == 0, "component '{}' added off the main thread", name());
        GX_CHECK(entity.isValid(), "adding component '{}' to an invalid entity", name());
        GX_CHECK(!contains(entity), "entity {} already has component '{}'", entity.index, name());
        if (entity.index >= m_sparse.size()) {
            m_sparse.resize(static_cast<usize>(entity.index) + 1, kAbsent);
        }
        m_sparse[entity.index] = static_cast<u32>(m_values.size());
        m_entities.push_back(entity);
        m_values.push_back(std::move(value));
        return m_values.back();
    }

    void remove(EntityId entity) {
        GX_CHECK(contains(entity), "entity {} has no component '{}'", entity.index, name());
        removeIfPresent(entity);
    }

    void removeIfPresent(EntityId entity) override {
        GX_ASSERT(JobSystem::currentThreadIndex() == 0, "component '{}' removed off the main thread", name());
        if (!contains(entity)) {
            return;
        }
        const u32 slot = m_sparse[entity.index];
        const auto last = static_cast<u32>(m_values.size() - 1);
        if (slot != last) {
            m_values[slot] = std::move(m_values[last]);
            m_entities[slot] = m_entities[last];
            m_sparse[m_entities[slot].index] = slot;
        }
        m_values.pop_back();
        m_entities.pop_back();
        m_sparse[entity.index] = kAbsent;
    }

    [[nodiscard]] bool contains(EntityId entity) const override {
        return entity.index < m_sparse.size() && m_sparse[entity.index] != kAbsent &&
               m_entities[m_sparse[entity.index]] == entity;
    }

    [[nodiscard]] T& get(EntityId entity) {
        GX_ASSERT(contains(entity), "entity {} has no component '{}'", entity.index, name());
        return m_values[m_sparse[entity.index]];
    }
    [[nodiscard]] const T& get(EntityId entity) const {
        GX_ASSERT(contains(entity), "entity {} has no component '{}'", entity.index, name());
        return m_values[m_sparse[entity.index]];
    }
    [[nodiscard]] T* tryGet(EntityId entity) {
        return contains(entity) ? &m_values[m_sparse[entity.index]] : nullptr;
    }
    [[nodiscard]] const T* tryGet(EntityId entity) const {
        return contains(entity) ? &m_values[m_sparse[entity.index]] : nullptr;
    }

    [[nodiscard]] std::span<T> values() { return m_values; }
    [[nodiscard]] std::span<const T> values() const { return m_values; }
    [[nodiscard]] std::span<const EntityId> entities() const override { return m_entities; }
    [[nodiscard]] usize size() const override { return m_values.size(); }

    void clear() override {
        m_values.clear();
        m_entities.clear();
        m_sparse.clear();
    }

    void write(BinaryWriter& writer) const override {
        writer.writeU64(m_values.size());
        for (usize i = 0; i < m_values.size(); ++i) {
            writer.io(m_entities[i]);
            writer.io(m_values[i]);
        }
    }

    void read(BinaryReader& reader) override {
        clear();
        const u64 count = reader.readU64();
        if (count > reader.remaining()) {
            reader.fail("component count exceeds the remaining data");
        }
        for (u64 i = 0; i < count && reader.ok(); ++i) {
            EntityId entity;
            T value{};
            reader.io(entity);
            reader.io(value);
            if (!reader.ok()) {
                break;
            }
            if (!entity.isValid() || contains(entity)) {
                reader.fail("component store '" + name() + "': invalid or duplicate entity");
                break;
            }
            add(entity, std::move(value));
        }
    }

    void inspect(EntityId entity, FieldVisitor& visitor) const override {
        InspectArchive archive(visitor);
        archive.io(name(), get(entity));
    }

private:
    static constexpr u32 kAbsent = 0xffffffffu;

    std::vector<T> m_values;
    std::vector<EntityId> m_entities; // parallel to m_values
    std::vector<u32> m_sparse;        // entity index -> dense slot
};

} // namespace gx
