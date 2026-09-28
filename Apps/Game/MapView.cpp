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
constexpr ImU32 kWellColor = IM_COL32(150, 110, 200, 70);
constexpr ImU32 kTruthColor = IM_COL32(255, 90, 200, 150); // debug: real positions of other factions
constexpr ImU32 kHyperspaceColor = IM_COL32(200, 160, 255, 200);
constexpr ImU32 kTargetColor = IM_COL32(255, 70, 60, 255);
constexpr ImU32 kPlayerFireColor = IM_COL32(120, 255, 220, 230);
constexpr ImU32 kHostileFireColor = IM_COL32(255, 120, 60, 230);
constexpr ImU32 kMissColor = IM_COL32(200, 200, 200, 70);
constexpr f64 kSpeedOfLight = 299'792'458.0;

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
    case BodyKind::AsteroidField:
        return {IM_COL32(190, 160, 120, 255), 6.0f};
    case BodyKind::IceField:
        return {IM_COL32(170, 225, 250, 255), 6.0f};
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
        return tr("Estrella");
    case BodyKind::RockyPlanet:
        return tr("Planeta rocoso");
    case BodyKind::DesertPlanet:
        return tr("Planeta desértico");
    case BodyKind::OceanPlanet:
        return tr("Planeta oceánico");
    case BodyKind::IcePlanet:
        return tr("Planeta helado");
    case BodyKind::GasGiant:
        return tr("Gigante gaseoso");
    case BodyKind::Moon:
        return tr("Luna");
    case BodyKind::Station:
        return tr("Estación");
    case BodyKind::AsteroidField:
        return tr("Campo de asteroides");
    case BodyKind::IceField:
        return tr("Campo de hielo");
    default:
        return "?";
    }
}

const char* displayName(FlightMode mode) {
    switch (mode) {
    case FlightMode::Coast:
        return tr("Deriva");
    case FlightMode::Stop:
        return tr("Deteniéndose");
    case FlightMode::MoveTo:
        return tr("Rumbo a un punto");
    case FlightMode::Approach:
        return tr("Aproximación");
    case FlightMode::Manual:
        return tr("Pilotaje manual");
    case FlightMode::Pursue:
        return tr("Persecución");
    default:
        return "?";
    }
}

const char* displayName(DrivePhase phase) {
    switch (phase) {
    case DrivePhase::Sublight:
        return tr("Sublumínico");
    case DrivePhase::Charging:
        return tr("Cargando salto");
    case DrivePhase::Hyperspace:
        return tr("Hiperespacio");
    default:
        return "?";
    }
}

const char* displayName(ContactLevel level) {
    switch (level) {
    case ContactLevel::Unknown:
        return tr("Desconocido");
    case ContactLevel::Classified:
        return tr("Clasificado");
    case ContactLevel::Identified:
        return tr("Identificado");
    default:
        return "?";
    }
}

const char* displayName(ModuleType type) {
    const auto index = static_cast<usize>(type);
    return index < content::kModuleNames.size() ? tr(content::kModuleNames[index]) : "?";
}

std::string localizedName(std::string_view name) {
    return render(properName(std::string(name)), activeCatalog());
}

std::string contactLabel(const ContactView& contact) {
    if (contact.level == ContactLevel::Identified && !contact.name.empty()) {
        return std::string(contact.name);
    }
    if (contact.level == ContactLevel::Classified) {
        return std::format("{}? ({})", tr(content::kShipClasses[contact.shipClass].name), contact.trackId);
    }
    return std::format("?{}", contact.trackId);
}

std::string formatDistance(f64 meters) {
    if (meters < 1'000.0) {
        return std::format("{:.0f} m", meters);
    }
    if (meters < 1e9) {
        return std::format("{:.0f} km", meters / 1'000.0);
    }
    return std::format("{:.3f} {}", meters / kAstronomicalUnit, tr("UA"));
}

std::string formatSpeed(f64 metersPerSecond) {
    if (metersPerSecond >= 0.1 * kSpeedOfLight) {
        return std::format("{:.0f} km/s ({:.2f} c)", metersPerSecond / 1'000.0,
                           metersPerSecond / kSpeedOfLight);
    }
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

MapSelection MapView::pick(const SystemSnapshot& snapshot, ImVec2 screen, const MapOptions& options) const {
    MapSelection best;
    float bestDistanceSq = kPickRadius * kPickRadius;
    // Ships and contacts first: they sit on top of the bodies they orbit and are what the player usually
    // wants.
    for (const ShipView& ship : snapshot.ships) {
        if (ship.faction != snapshot.playerFaction && !options.showTruth) {
            continue; // other factions are only known through sensors
        }
        const float d = distanceSq(toScreen(ship.position), screen);
        if (d < bestDistanceSq) {
            bestDistanceSq = d;
            best = {ship.id, 0};
        }
    }
    for (const ContactView& contact : snapshot.contacts) {
        const float d = distanceSq(toScreen(contact.position), screen);
        if (d < bestDistanceSq) {
            bestDistanceSq = d;
            best = {{}, contact.trackId};
        }
    }
    if (!best.empty()) {
        return best;
    }
    for (const BodyView& body : snapshot.bodies) {
        const float radius = std::max(static_cast<float>(body.radius / m_camera.metersPerPixel), kPickRadius);
        const float d = distanceSq(toScreen(body.position), screen);
        if (d < radius * radius && (best.empty() || d < bestDistanceSq)) {
            bestDistanceSq = d;
            best = {body.id, 0};
        }
    }
    return best;
}

MapView::Interaction MapView::update(const SystemSnapshot& snapshot, const MapSelection& selected,
                                     const MapOptions& options) {
    const ImGuiIO& io = ImGui::GetIO();
    m_viewSize = io.DisplaySize;
    m_viewCenter = {io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f};

    Vec3d followed;
    if (m_camera.follow.isValid() && snapshot.positionOf(m_camera.follow, followed)) {
        m_camera.center = followed;
    }

    Interaction interaction;
    MapSelection hovered;
    if (!io.WantCaptureMouse) {
        const ImVec2 mouse = io.MousePos;
        hovered = pick(snapshot, mouse, options);

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
                interaction.leftClickedTarget = hovered;
            }
            m_panning = false;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            interaction.rightClicked = true;
            interaction.rightClickedTarget = hovered;
            interaction.rightClickedPoint = toWorld(mouse);
        }
    } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_panning = false;
    }

    draw(snapshot, selected, hovered, options);
    return interaction;
}

void MapView::draw(const SystemSnapshot& snapshot, const MapSelection& selected, const MapSelection& hovered,
                   const MapOptions& options) {
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

    // Gravity wells: dashed rings where hyperspace is not allowed (only when they are a useful size on
    // screen).
    for (const BodyView& body : snapshot.bodies) {
        const f64 screenRadius = body.wellRadius / mpp;
        if (body.wellRadius <= 0.0 || screenRadius < 12.0 || screenRadius > 2e5) {
            continue;
        }
        const ImVec2 center = toScreen(body.position);
        constexpr int kDashes = 48;
        for (int i = 0; i < kDashes; i += 2) {
            const f32 a0 = static_cast<f32>(kTwoPi * i / kDashes);
            const f32 a1 = static_cast<f32>(kTwoPi * (i + 1) / kDashes);
            const auto r = static_cast<float>(screenRadius);
            drawList.AddLine({center.x + r * std::cos(a0), center.y + r * std::sin(a0)},
                             {center.x + r * std::cos(a1), center.y + r * std::sin(a1)}, kWellColor, 1.0f);
        }
    }

    // Bodies.
    for (const BodyView& body : snapshot.bodies) {
        const BodyStyle style = styleOf(body.kind);
        const ImVec2 center = toScreen(body.position);
        const float radius =
            std::clamp(static_cast<float>(body.radius / mpp), style.minRadius, kMaxDrawRadius);
        if (isAsteroidField(body.kind)) {
            // A loose cluster of rocks (or ice): a few dots in a fixed pattern, dimmer as the deposit runs
            // out.
            const DepositView* deposit = snapshot.findDeposit(body.id);
            const f64 fullness =
                deposit != nullptr && deposit->size > 0.0 ? deposit->reserve / deposit->size : 1.0;
            const ImU32 rock = (style.color & 0x00FFFFFFu) | (static_cast<ImU32>(90 + 165 * fullness) << 24);
            constexpr std::array<std::array<float, 3>, 7> kRocks = {{{0.0f, 0.0f, 0.45f},
                                                                     {0.7f, 0.3f, 0.3f},
                                                                     {-0.6f, 0.5f, 0.35f},
                                                                     {-0.3f, -0.7f, 0.3f},
                                                                     {0.5f, -0.6f, 0.25f},
                                                                     {-0.9f, -0.1f, 0.2f},
                                                                     {0.2f, 0.9f, 0.2f}}};
            for (const auto& [x, y, size] : kRocks) {
                drawList.AddCircleFilled({center.x + x * radius, center.y + y * radius},
                                         std::max(1.5f, size * radius), rock);
            }
        } else if (body.kind == BodyKind::Station && radius <= style.minRadius) {
            drawList.AddRectFilled({center.x - radius, center.y - radius},
                                   {center.x + radius, center.y + radius}, style.color);
        } else {
            drawList.AddCircleFilled(center, radius, style.color);
        }
        if (body.id == selected.entity || body.id == hovered.entity) {
            drawList.AddCircle(center, radius + 4.0f, body.id == selected.entity ? kSelectionColor : kDimText,
                               0, 1.5f);
        }
        // Labels: stars and planets always; moons and stations once their orbit is visibly large.
        const bool isMinor = body.kind == BodyKind::Moon || body.kind == BodyKind::Station;
        const bool orbitVisible = body.orbitPath != nullptr && !body.orbitPath->empty() &&
                                  length(body.orbitPath->front()) / mpp > 25.0;
        if (!isMinor || orbitVisible || body.id == selected.entity || body.id == hovered.entity) {
            addText(drawList, {center.x + radius + 4.0f, center.y - 7.0f},
                    isMinor ? kDimText : kBackgroundText, localizedName(body.name));
        }
    }

    drawContacts(drawList, snapshot, selected, hovered, options);
    drawCombat(drawList, snapshot, options);

    // Own ships (and, when debugging, every ship where it really is): course line, thrust plume, hull
    // triangle pointing along the velocity.
    for (const ShipView& ship : snapshot.ships) {
        const bool own = ship.faction == snapshot.playerFaction;
        if (!own && !options.showTruth) {
            continue;
        }
        const ImVec2 center = toScreen(ship.position);
        const ImU32 color = ship.isPlayer ? kPlayerColor : (own ? kHaulerColor : kTruthColor);
        if (ship.phase == DrivePhase::Hyperspace) {
            // Streak behind the ship, along its motion.
            const f64 speed = length(ship.velocity);
            if (speed > 0.0) {
                const ImVec2 back{static_cast<float>(-ship.velocity.x / speed),
                                  static_cast<float>(ship.velocity.y / speed)};
                drawList.AddLine(center, {center.x + back.x * 40.0f, center.y + back.y * 40.0f},
                                 kHyperspaceColor, 3.0f);
            }
        } else if (ship.phase == DrivePhase::Charging) {
            drawList.AddCircle(center, 11.0f, kHyperspaceColor, 0, 1.5f);
        }
        const bool isSelected = ship.id == selected.entity;
        const bool isHovered = ship.id == hovered.entity;
        const bool travelling =
            (ship.mode == FlightMode::MoveTo || ship.mode == FlightMode::Approach) && !ship.arrived;
        if (travelling && (ship.isPlayer || isSelected)) {
            drawList.AddLine(center, toScreen(ship.targetPosition),
                             ship.isPlayer ? kCourseColor : kTruthColor, 1.0f);
        }
        if (ship.isPlayer && ship.mode == FlightMode::Pursue) {
            if (const ContactView* chased = snapshot.findContact(ship.track)) {
                drawList.AddLine(center, toScreen(chased->position), kCourseColor, 1.0f);
            }
        }
        const f64 headingLength = length(ship.velocity);
        const ImVec2 forward = headingLength > 1.0
                                   ? ImVec2{static_cast<float>(ship.velocity.x / headingLength),
                                            static_cast<float>(-ship.velocity.y / headingLength)}
                                   : ImVec2{0.0f, -1.0f};
        const ImVec2 side{-forward.y, forward.x};
        const float size = ship.isPlayer ? 8.0f : 6.0f;
        const f64 thrust = length(ship.acceleration);
        if (thrust > 1e-3 && own) {
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
        if (isSelected || isHovered) {
            drawList.AddCircle(center, size + 5.0f, isSelected ? kSelectionColor : kDimText, 0, 1.5f);
        }
        if (ship.isPlayer || isSelected || isHovered || (own && mpp < 2'000.0)) {
            addText(drawList, {center.x + size + 4.0f, center.y + 2.0f}, color, ship.name);
        }
        if (!ship.powered && (own || options.showTruth)) {
            addText(drawList, {center.x + size + 4.0f, center.y + 16.0f}, kTargetColor, tr("sin energía"));
        }
    }

    drawScaleBar(drawList);
}

void MapView::drawContacts(ImDrawList& drawList, const SystemSnapshot& snapshot, const MapSelection& selected,
                           const MapSelection& hovered, const MapOptions& options) {
    const f64 mpp = m_camera.metersPerPixel;
    u32 fireTrack = 0;
    for (const ShipView& ship : snapshot.ships) {
        if (ship.isPlayer) {
            fireTrack = ship.fireTrack;
        }
    }
    for (const ContactView& contact : snapshot.contacts) {
        const ImVec2 center = toScreen(contact.position);
        // Fresh tracks are bright; stale ones fade until they are lost.
        const auto alpha = static_cast<u8>(contact.ageSeconds < 2.0 ? 235 : 110);
        ImU32 color = IM_COL32(190, 190, 190, alpha); // unknown
        if (contact.level == ContactLevel::Classified) {
            color = IM_COL32(235, 200, 120, alpha);
        } else if (contact.level == ContactLevel::Identified) {
            color = contact.faction == content::kFactionIndependent ? IM_COL32(120, 170, 255, alpha)
                    : contact.faction == content::kFactionAuthority ? IM_COL32(210, 235, 255, alpha)
                                                                    : IM_COL32(255, 110, 100, alpha);
        }
        const f64 sigmaPixels = contact.uncertainty / mpp;
        if (sigmaPixels > 4.0 && sigmaPixels < 1e5) {
            drawList.AddCircle(center, static_cast<float>(sigmaPixels), IM_COL32(190, 190, 190, 60), 0, 1.0f);
        }
        constexpr float kSize = 6.0f;
        drawList.AddQuad({center.x, center.y - kSize}, {center.x + kSize, center.y},
                         {center.x, center.y + kSize}, {center.x - kSize, center.y}, color, 1.5f);
        if (contact.level == ContactLevel::Identified) {
            drawList.AddCircleFilled(center, 2.0f, color);
        }
        const bool isSelected = selected.contact == contact.trackId;
        if (isSelected || hovered.contact == contact.trackId) {
            drawList.AddCircle(center, kSize + 5.0f, isSelected ? kSelectionColor : kDimText, 0, 1.5f);
        }
        if (contact.trackId == fireTrack) {
            // The player's weapons are on this track: a reticle.
            constexpr float kReticle = 13.0f;
            drawList.AddCircle(center, kReticle, kTargetColor, 0, 1.5f);
            drawList.AddLine({center.x - kReticle - 5.0f, center.y}, {center.x - kReticle + 4.0f, center.y},
                             kTargetColor, 1.5f);
            drawList.AddLine({center.x + kReticle - 4.0f, center.y}, {center.x + kReticle + 5.0f, center.y},
                             kTargetColor, 1.5f);
            drawList.AddLine({center.x, center.y - kReticle - 5.0f}, {center.x, center.y - kReticle + 4.0f},
                             kTargetColor, 1.5f);
            drawList.AddLine({center.x, center.y + kReticle - 4.0f}, {center.x, center.y + kReticle + 5.0f},
                             kTargetColor, 1.5f);
        }
        std::string label = contactLabel(contact);
        if (options.showTruth && contact.ghost) {
            label += std::string(" ") + tr("[fantasma]");
        }
        addText(drawList, {center.x + kSize + 4.0f, center.y - 7.0f}, color, label);
    }
}

void MapView::drawCombat(ImDrawList& drawList, const SystemSnapshot& snapshot,
                         const MapOptions& options) const {
    // Beams: bright while they connect, faint when they miss. Only fire the player could see, unless
    // debugging.
    for (const BeamView& beam : snapshot.beams) {
        if (!beam.visible && !options.showTruth) {
            continue;
        }
        const ImU32 color = !beam.hit ? kMissColor : (beam.byPlayer ? kPlayerFireColor : kHostileFireColor);
        drawList.AddLine(toScreen(beam.from), toScreen(beam.to), color, beam.hit ? 2.0f : 1.0f);
    }
    // Slugs: a dot with a short streak along their motion (a fixed number of pixels: they are too fast).
    for (const ProjectileView& projectile : snapshot.projectiles) {
        if (!projectile.visible && !options.showTruth) {
            continue;
        }
        const ImVec2 at = toScreen(projectile.position);
        const f64 speed = length(projectile.velocity);
        const ImU32 color = projectile.byPlayer ? kPlayerFireColor : kHostileFireColor;
        if (speed > 0.0) {
            const ImVec2 back{static_cast<float>(-projectile.velocity.x / speed),
                              static_cast<float>(projectile.velocity.y / speed)};
            drawList.AddLine(at, {at.x + back.x * 12.0f, at.y + back.y * 12.0f}, color, 1.5f);
        }
        drawList.AddCircleFilled(at, 2.0f, color);
    }
    // Explosions: an expanding, fading ring.
    for (const ExplosionView& explosion : snapshot.explosions) {
        if (!explosion.visible && !options.showTruth) {
            continue;
        }
        const f64 life = CombatSystem::kExplosionVisibleFor.toSeconds();
        const f64 progress = std::clamp(explosion.ageSeconds / life, 0.0, 1.0);
        const auto alpha = static_cast<u8>(255.0 * (1.0 - progress));
        const ImVec2 at = toScreen(explosion.position);
        drawList.AddCircleFilled(at, static_cast<float>(4.0 + 6.0 * (1.0 - progress)),
                                 IM_COL32(255, 230, 160, alpha));
        drawList.AddCircle(at, static_cast<float>(6.0 + 30.0 * progress), IM_COL32(255, 150, 60, alpha), 0,
                           2.0f);
    }
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
            astronomical ? std::format("{:g} {}", niceUnits, tr("UA")) : formatDistance(nice));
}

} // namespace gx
