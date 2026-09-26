#pragma once

#include "Simulation/World/Inspect.h"

#include <vector>

namespace gx {

class World;

// Entity Inspector (prompt §26): every component of an entity as an ImGui tree, generated from the same
// `io` descriptions that serialization uses. Entity references are buttons that select the referenced entity.
class InspectorView final : public FieldVisitor {
public:
    // Returns the entity reference the user clicked this frame, or an invalid id.
    EntityId draw(const World& world, EntityId entity);

    void beginGroup(std::string_view name) override;
    void endGroup() override;
    void field(std::string_view name, std::string_view formattedValue) override;
    void entityField(std::string_view name, EntityId entity) override;

private:
    [[nodiscard]] bool visible() const;

    std::vector<bool> m_open; // one entry per group: whether its tree node is open (and must be popped)
    EntityId m_clicked;
    int m_widgetId = 0;
};

} // namespace gx
