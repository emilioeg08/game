#include "Apps/Game/InspectorView.h"

#include "Engine/Text/Localization.h"

#include "Simulation/World/World.h"

#include <imgui.h>

#include <format>
#include <string>

namespace gx {

EntityId InspectorView::draw(const World& world, EntityId entity) {
    m_open.clear();
    m_clicked = {};
    m_widgetId = 0;
    if (!world.isAlive(entity)) {
        ImGui::TextDisabled("%s", tr("(la entidad ya no existe)"));
        return {};
    }
    ImGui::TextDisabled(tr("Entidad #%u (generación %u)"), entity.index, entity.generation);
    world.inspect(entity, *this);
    return m_clicked;
}

bool InspectorView::visible() const {
    for (const bool open : m_open) {
        if (!open) {
            return false;
        }
    }
    return true;
}

void InspectorView::beginGroup(std::string_view name) {
    const bool open =
        visible() && ImGui::TreeNodeEx(std::string(name).c_str(), ImGuiTreeNodeFlags_DefaultOpen);
    m_open.push_back(open);
}

void InspectorView::endGroup() {
    const bool wasOpen = m_open.back();
    m_open.pop_back();
    if (wasOpen) {
        ImGui::TreePop();
    }
}

void InspectorView::field(std::string_view name, std::string_view formattedValue) {
    if (!visible()) {
        return;
    }
    ImGui::TextDisabled("%.*s:", static_cast<int>(name.size()), name.data());
    ImGui::SameLine();
    ImGui::TextUnformatted(formattedValue.data(), formattedValue.data() + formattedValue.size());
}

void InspectorView::entityField(std::string_view name, EntityId entity) {
    if (!visible()) {
        return;
    }
    ImGui::TextDisabled("%.*s:", static_cast<int>(name.size()), name.data());
    ImGui::SameLine();
    if (!entity.isValid()) {
        ImGui::TextUnformatted("-");
        return;
    }
    ImGui::PushID(m_widgetId++);
    if (ImGui::SmallButton(std::format("#{}:{}", entity.index, entity.generation).c_str())) {
        m_clicked = entity;
    }
    ImGui::PopID();
}

} // namespace gx
