#pragma once

#include "Engine/Math/Vec3.h"
#include "Game/Presentation/SystemSnapshot.h"
#include "Simulation/World/EntityRegistry.h"

#include <imgui.h>

namespace gx {

// 2D map of the system in the ecliptic plane (x right, y up). The camera works in doubles and converts to
// screen floats relative to its centre, which is origin rebasing: precision holds from metres to tens of AU.
struct Camera {
    Vec3d center;
    f64 metersPerPixel = 50'000.0;
    EntityId follow;
};

class MapView {
public:
    struct Interaction {
        bool leftClicked = false;
        EntityId leftClickedEntity; // invalid: clicked empty space
        bool rightClicked = false;
        EntityId rightClickedEntity;
        Vec3d rightClickedPoint;
    };

    // Handles mouse input over the map (when ImGui does not want it) and draws the snapshot into the
    // background draw list.
    Interaction update(const SystemSnapshot& snapshot, EntityId selected);

    [[nodiscard]] Camera& camera() { return m_camera; }
    [[nodiscard]] ImVec2 toScreen(const Vec3d& position) const;
    [[nodiscard]] Vec3d toWorld(ImVec2 screen) const;

private:
    [[nodiscard]] EntityId pick(const SystemSnapshot& snapshot, ImVec2 screen) const;
    void draw(const SystemSnapshot& snapshot, EntityId selected, EntityId hovered);
    void drawScaleBar(ImDrawList& drawList) const;

    Camera m_camera;
    ImVec2 m_viewCenter{640.0f, 360.0f};
    ImVec2 m_viewSize{1280.0f, 720.0f};
    bool m_panning = false;
};

// Display names (Spanish UI) for simulation enums.
[[nodiscard]] const char* displayName(BodyKind kind);
[[nodiscard]] const char* displayName(FlightMode mode);
[[nodiscard]] std::string formatDistance(f64 meters);
[[nodiscard]] std::string formatSpeed(f64 metersPerSecond);

} // namespace gx
