#pragma once

#include "Engine/Math/Vec3.h"
#include "Engine/Text/Localization.h"
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

// What can be selected on the map: a real entity (body or own ship) or a sensor contact (track id).
struct MapSelection {
    EntityId entity;
    u32 contact = 0;

    [[nodiscard]] bool empty() const { return !entity.isValid() && contact == 0; }
    bool operator==(const MapSelection&) const = default;
};

struct MapOptions {
    bool showTruth = false; // debug: draw every ship where it really is, and mark ghost contacts
};

class MapView {
public:
    struct Interaction {
        bool leftClicked = false;
        MapSelection leftClickedTarget; // empty: clicked empty space
        bool rightClicked = false;
        MapSelection rightClickedTarget;
        Vec3d rightClickedPoint;
    };

    // Handles mouse input over the map (when ImGui does not want it) and draws the snapshot into the
    // background draw list. Other factions' ships appear only as sensor contacts (unless showTruth).
    Interaction update(const SystemSnapshot& snapshot, const MapSelection& selected,
                       const MapOptions& options);

    [[nodiscard]] Camera& camera() { return m_camera; }
    [[nodiscard]] ImVec2 toScreen(const Vec3d& position) const;
    [[nodiscard]] Vec3d toWorld(ImVec2 screen) const;

private:
    [[nodiscard]] MapSelection pick(const SystemSnapshot& snapshot, ImVec2 screen,
                                    const MapOptions& options) const;
    void draw(const SystemSnapshot& snapshot, const MapSelection& selected, const MapSelection& hovered,
              const MapOptions& options);
    void drawContacts(ImDrawList& drawList, const SystemSnapshot& snapshot, const MapSelection& selected,
                      const MapSelection& hovered, const MapOptions& options);
    void drawCombat(ImDrawList& drawList, const SystemSnapshot& snapshot, const MapOptions& options) const;
    void drawScaleBar(ImDrawList& drawList) const;

    Camera m_camera;
    ImVec2 m_viewCenter{640.0f, 360.0f};
    ImVec2 m_viewSize{1280.0f, 720.0f};
    bool m_panning = false;
};

// Display names (Spanish UI) for simulation enums.
[[nodiscard]] const char* displayName(BodyKind kind);
[[nodiscard]] const char* displayName(FlightMode mode);
[[nodiscard]] const char* displayName(DrivePhase phase);
[[nodiscard]] const char* displayName(ContactLevel level);
[[nodiscard]] const char* displayName(ModuleType type);
// Label of a contact as the player knows it: name when identified, class when classified, track otherwise.
[[nodiscard]] std::string contactLabel(const ContactView& contact);
// A body's name in the active language: its title ("Estación", "Puerto"...) is translated.
[[nodiscard]] std::string localizedName(std::string_view name);
[[nodiscard]] std::string formatDistance(f64 meters);
[[nodiscard]] std::string formatSpeed(f64 metersPerSecond);

} // namespace gx
