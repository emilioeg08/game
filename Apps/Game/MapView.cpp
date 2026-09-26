#include "Apps/Game/MapView.h"

#include "Game/Sandbox/Content.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

namespace gx {
namespace {

constexpr f64 kAstronomicalUnit = 1.495978707e11;
constexpr f64 kMinMetersPerPixel = 1.0;
constexpr f64 kMaxMetersPerPixel = 5e10;
constexpr float kPickRadius = 12.0f;
constexpr float kMaxDrawRadius = 1e5f; // beyond this, geometry is far off-screen anyway

constexpr ImU32 kBackgroundText = IM_COL32(200, 205, 215, 255);
constexpr ImU32 kDimText = IM_COL32(140, 148, 160, 255);
constexpr ImU32 kOrbitColor = IM_COL32(70, 82, 105, 170);
constexpr ImU32 kSelectionColor = IM_COL32(255, 214, 90, 255);
constexpr ImU32 kPlayerColor = IM_COL32(120, 255, 150, 255);
constexpr ImU32 kHaulerColor = IM_COL32(120, 170, 255, 255);
constexpr ImU32 kCourseColor = IM_COL32(120, 255, 150, 90);
constexpr ImU32 kThrustColor = IM_COL32(255, 150, 70, 220);

struct BodyStyle {
    ImU32 color;
    float minRadius; // px
};

BodyStyle styleOf(BodyKind kind) {
    switch (kind) {
    case BodyKind::Star:
        return {IM_COL32(255, 222, 130, 255), 7.0f};
    case BodyKind::RockyPlanet:
        return {IM_COL32(170, 150, 130, 255), 4.0f};
    case BodyKind::DesertPlanet:
        return {IM_COL32(214, 172, 102, 255), 4.0f};
    case BodyKind::OceanPlanet:
        return {IM_COL32(80, 145, 225, 255), 4.5f};
    case BodyKind::IcePlanet:
        return {IM_COL32(185, 215, 235, 255), 4.0f};
    case BodyKind::GasGiant:
        return {IM_COL32(205, 165, 115, 255), 5.5f};
    case BodyKind::Moon:
        return {IM_COL32(150, 150, 150, 255), 2.5f};
    case BodyKind::Station:
        return {IM_COL32(110, 225, 205, 255), 3.0f};
    default:
        return {IM_COL32(255, 255, 255, 255), 3.0f};
    }
}

float distanceSq(ImVec2 a, ImVec2 b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return dx * dx + dy * dy;
}

void addText(ImDrawList& drawList, ImVec2 position, ImU32 color, std::string_view text) {
    drawList.AddText(position, color, text.data(), text.data() + text.size());
}

} // namespace

const char* displayName(BodyKind kind) {
    switch (kind) {
    case BodyKind::Star:
        return "Estrella";
    case BodyKind::RockyPlanet:
        return "Planeta rocoso";
    case BodyKind::DesertPlanet:
        return "Planeta desértico";
    case BodyKind::OceanPlanet:
        return "Planeta oceánico";
    case BodyKind::IcePlanet:
        return "Planeta helado";
    case BodyKind::GasGiant:
        return "Gigante gaseoso";
    case BodyKind::Moon:
        return "Luna";
    case BodyKind::Station:
        return "Estación";
    default:
        return "?";
    }
}

const char* displayName(FlightMode mode) {
    switch (mode) {
    case FlightMode::Coast:
        return "Deriva";
    case FlightMode::Stop:
        return "Deteniéndose";
    case FlightMode::MoveTo:
        return "Rumbo a un punto";
    case FlightMode::Approach:
        return "Aproximación";
    case FlightMode::Manual:
        return "Pilotaje manual";
    default:
        return "?";
    }
}

std::string formatDistance(f64 meters) {
    if (meters < 1'000.0) {
        return std::format("{:.0f} m", meters);
    }
    if (meters < 1e9) {
        return std::format("{:.0f} km", meters / 1'000.0);
    }
    return std::format("{:.3f} UA", meters / kAstronomicalUnit);
}

std::string formatSpeed(f64 metersPerSecond) {
    return std::format("{:.1f} km/s", metersPerSecond / 1'000.0);
}

ImVec2 MapView::toScreen(const Vec3d& position) const {
    return {static_cast<float>(m_viewCenter.x + (position.x - m_camera.center.x) / m_camera.metersPerPixel),
            static_cast<float>(m_viewCenter.y - (position.y - m_camera.center.y) / m_camera.metersPerPixel)};
}

Vec3d MapView::toWorld(ImVec2 screen) const {
    return {m_camera.center.x + (screen.x - m_viewCenter.x) * m_camera.metersPerPixel,
            m_camera.center.y - (screen.y - m_viewCenter.y) * m_camera.metersPerPixel, 0.0};
}

EntityId MapView::pick(const SystemSnapshot& snapshot, ImVec2 screen) const {
    EntityId best;
    float bestDistanceSq = kPickRadius * kPickRadius;
    // Ships first: they sit on top of the bodies they orbit and are what the player usually wants.
    for (const ShipView& ship : snapshot.ships) {
        const float d = distanceSq(toScreen(ship.position), screen);
        if (d < bestDistanceSq) {
            bestDistanceSq = d;
            best = ship.id;
        }
    }
    if (best.isValid()) {
        return best;
    }
    for (const BodyView& body : snapshot.bodies) {
        const float radius = std::max(static_cast<float>(body.radius / m_camera.metersPerPixel), kPickRadius);
        const float d = distanceSq(toScreen(body.position), screen);
        if (d < radius * radius && (!best.isValid() || d < bestDistanceSq)) {
            bestDistanceSq = d;
            best = body.id;
        }
    }
    return best;
}

MapView::Interaction MapView::update(const SystemSnapshot& snapshot, EntityId selected) {
    const ImGuiIO& io = ImGui::GetIO();
    m_viewSize = io.DisplaySize;
    m_viewCenter = {io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f};

    Vec3d followed;
    if (m_camera.follow.isValid() && snapshot.positionOf(m_camera.follow, followed)) {
        m_camera.center = followed;
    }

    Interaction interaction;
    EntityId hovered;
    if (!io.WantCaptureMouse) {
        const ImVec2 mouse = io.MousePos;
        hovered = pick(snapshot, mouse);

        if (io.MouseWheel != 0.0f) {
            const Vec3d before = toWorld(mouse);
            m_camera.metersPerPixel = std::clamp(m_camera.metersPerPixel * std::pow(1.25, -io.MouseWheel),
                                                 kMinMetersPerPixel, kMaxMetersPerPixel);
            if (!m_camera.follow.isValid()) {
                const Vec3d after = toWorld(mouse); // keep the point under the cursor fixed
                m_camera.center += before - after;
            }
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f)) {
            m_panning = true;
            m_camera.follow = {};
            m_camera.center.x -= io.MouseDelta.x * m_camera.metersPerPixel;
            m_camera.center.y += io.MouseDelta.y * m_camera.metersPerPixel;
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (!m_panning) {
                interaction.leftClicked = true;
                interaction.leftClickedEntity = hovered;
            }
            m_panning = false;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            interaction.rightClicked = true;
            interaction.rightClickedEntity = hovered;
            interaction.rightClickedPoint = toWorld(mouse);
        }
    } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_panning = false;
    }

    draw(snapshot, selected, hovered);
    return interaction;
}

void MapView::draw(const SystemSnapshot& snapshot, EntityId selected, EntityId hovered) {
    ImDrawList& drawList = *ImGui::GetBackgroundDrawList();
    const f64 mpp = m_camera.metersPerPixel;

    // Orbits (skipped when too small to see or so large that only a straight sliver would be visible).
    for (const BodyView& body : snapshot.bodies) {
        if (body.orbitPath == nullptr || body.orbitPath->empty()) {
            continue;
        }
        const f64 screenRadius = length(body.orbitPath->front()) / mpp;
        if (screenRadius < 3.0 || screenRadius > 2e5) {
            continue;
        }
        std::array<ImVec2, SnapshotBuilder::kOrbitPathPoints> points{};
        const usize count = std::min(points.size(), body.orbitPath->size());
        for (usize i = 0; i < count; ++i) {
            points[i] = toScreen(body.parentPosition + (*body.orbitPath)[i]);
        }
        drawList.AddPolyline(points.data(), static_cast<int>(count), kOrbitColor, 1.0f, ImDrawFlags_Closed);
    }

    // Bodies.
    for (const BodyView& body : snapshot.bodies) {
        const BodyStyle style = styleOf(body.kind);
        const ImVec2 center = toScreen(body.position);
        const float radius =
            std::clamp(static_cast<float>(body.radius / mpp), style.minRadius, kMaxDrawRadius);
        if (body.kind == BodyKind::Station && radius <= style.minRadius) {
            drawList.AddRectFilled({center.x - radius, center.y - radius},
                                   {center.x + radius, center.y + radius}, style.color);
        } else {
            drawList.AddCircleFilled(center, radius, style.color);
        }
        if (body.id == selected || body.id == hovered) {
            drawList.AddCircle(center, radius + 4.0f, body.id == selected ? kSelectionColor : kDimText, 0,
                               1.5f);
        }
        // Labels: stars and planets always; moons and stations once their orbit is visibly large.
        const bool isMinor = body.kind == BodyKind::Moon || body.kind == BodyKind::Station;
        const bool orbitVisible = body.orbitPath != nullptr && !body.orbitPath->empty() &&
                                  length(body.orbitPath->front()) / mpp > 25.0;
        if (!isMinor || orbitVisible || body.id == selected || body.id == hovered) {
            addText(drawList, {center.x + radius + 4.0f, center.y - 7.0f},
                    isMinor ? kDimText : kBackgroundText, body.name);
        }
    }

    // Ships: course line, thrust plume, hull triangle pointing along the velocity.
    for (const ShipView& ship : snapshot.ships) {
        const ImVec2 center = toScreen(ship.position);
        const ImU32 color = ship.isPlayer ? kPlayerColor : kHaulerColor;
        const bool travelling =
            (ship.mode == FlightMode::MoveTo || ship.mode == FlightMode::Approach) && !ship.arrived;
        if (travelling && (ship.isPlayer || ship.id == selected)) {
            drawList.AddLine(center, toScreen(ship.targetPosition),
                             ship.isPlayer ? kCourseColor : IM_COL32(120, 170, 255, 90), 1.0f);
        }
        Vec3d heading = ship.velocity;
        const f64 headingLength = length(heading);
        const ImVec2 forward = headingLength > 1.0 ? ImVec2{static_cast<float>(heading.x / headingLength),
                                                            static_cast<float>(-heading.y / headingLength)}
                                                   : ImVec2{0.0f, -1.0f};
        const ImVec2 side{-forward.y, forward.x};
        const float size = ship.isPlayer ? 8.0f : 6.0f;
        const f64 thrust = length(ship.acceleration);
        if (thrust > 1e-3) {
            const ImVec2 plume{static_cast<float>(-ship.acceleration.x / thrust),
                               static_cast<float>(ship.acceleration.y / thrust)};
            drawList.AddLine(center, {center.x + plume.x * size * 1.8f, center.y + plume.y * size * 1.8f},
                             kThrustColor, 2.0f);
        }
        drawList.AddTriangleFilled({center.x + forward.x * size, center.y + forward.y * size},
                                   {center.x - forward.x * size * 0.6f + side.x * size * 0.6f,
                                    center.y - forward.y * size * 0.6f + side.y * size * 0.6f},
                                   {center.x - forward.x * size * 0.6f - side.x * size * 0.6f,
                                    center.y - forward.y * size * 0.6f - side.y * size * 0.6f},
                                   color);
        if (ship.id == selected || ship.id == hovered) {
            drawList.AddCircle(center, size + 5.0f, ship.id == selected ? kSelectionColor : kDimText, 0,
                               1.5f);
        }
        if (ship.isPlayer || ship.id == selected || ship.id == hovered || mpp < 2'000.0) {
            addText(drawList, {center.x + size + 4.0f, center.y + 2.0f}, color, ship.name);
        }
    }

    drawScaleBar(drawList);
}

void MapView::drawScaleBar(ImDrawList& drawList) const {
    // Longest "nice" length (1, 2 or 5 x 10^n) that fits in ~180 px, in AU at system scale, metres below.
    const f64 target = m_camera.metersPerPixel * 180.0;
    const bool astronomical = target >= 0.01 * kAstronomicalUnit;
    const f64 unit = astronomical ? kAstronomicalUnit : 1.0;
    const f64 magnitude = std::pow(10.0, std::floor(std::log10(target / unit)));
    f64 niceUnits = magnitude;
    for (const f64 step : {5.0, 2.0, 1.0}) {
        if (step * magnitude * unit <= target) {
            niceUnits = step * magnitude;
            break;
        }
    }
    const f64 nice = niceUnits * unit;
    const float pixels = static_cast<float>(nice / m_camera.metersPerPixel);
    const ImVec2 origin{20.0f, m_viewSize.y - 24.0f};
    drawList.AddLine(origin, {origin.x + pixels, origin.y}, kBackgroundText, 1.5f);
    drawList.AddLine({origin.x, origin.y - 4.0f}, {origin.x, origin.y + 4.0f}, kBackgroundText, 1.5f);
    drawList.AddLine({origin.x + pixels, origin.y - 4.0f}, {origin.x + pixels, origin.y + 4.0f},
                     kBackgroundText, 1.5f);
    addText(drawList, {origin.x, origin.y - 20.0f}, kBackgroundText,
            astronomical ? std::format("{:g} UA", niceUnits) : formatDistance(nice));
}

} // namespace gx
