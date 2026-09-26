#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/World/EntityRegistry.h"
#include "Space/Bodies/CelestialBody.h"
#include "Space/Sensors/Sensors.h"
#include "Space/Ships/Ship.h"

#include <string_view>
#include <unordered_map>
#include <vector>

namespace gx {

class Sandbox;
class Simulation;

struct BodyView {
    EntityId id;
    EntityId parent;
    std::string_view name;
    BodyKind kind = BodyKind::RockyPlanet;
    f64 radius = 0.0;
    f64 wellRadius = 0.0; // gravity well (no hyperspace inside), 0 if none
    Vec3d position;
    Vec3d parentPosition;
    const std::vector<Vec3d>* orbitPath = nullptr; // closed polyline relative to the parent, or null
};

struct ShipView {
    EntityId id;
    std::string_view name;
    u32 faction = 0;
    u32 shipClass = 0;
    Vec3d position;
    Vec3d velocity;
    Vec3d acceleration;
    FlightMode mode = FlightMode::Coast;
    DrivePhase phase = DrivePhase::Sublight;
    f64 chargeRemaining = 0.0;
    EntityId target;
    Vec3d targetPosition; // where the autopilot is heading (valid for MoveTo/Approach)
    bool arrived = false;
    bool isPlayer = false;
};

// A ship as the player's sensors see it (never the truth, except what has been identified).
struct ContactView {
    u32 trackId = 0;
    ContactLevel level = ContactLevel::Unknown;
    Vec3d position; // extrapolated from the last detection with the estimated velocity
    Vec3d velocity;
    f64 uncertainty = 0.0;
    f64 ageSeconds = 0.0;  // since the last detection
    u32 shipClass = 0;     // valid from Classified
    u32 faction = 0;       // valid when Identified
    std::string_view name; // only when Identified
    bool ghost = false;    // DEBUG ONLY: the player cannot know this
};

// Read-only picture of the simulation for rendering, UI and debug tools: the presentation layer never touches
// live components. Views reference names owned by the World and are valid until the next simulation step.
struct SystemSnapshot {
    SimTime time;
    std::vector<BodyView> bodies;
    std::vector<ShipView> ships;       // ground truth: the client shows only its own fleet unless debugging
    std::vector<ContactView> contacts; // the player's sensor picture
    u32 playerFaction = 0;
    SensorSuite playerSensors; // the player's ship: radar/transponder switches
    f64 playerEmission = 0.0;  // how bright the player's ship is right now

    [[nodiscard]] const BodyView* findBody(EntityId id) const;
    [[nodiscard]] const ShipView* findShip(EntityId id) const;
    [[nodiscard]] const ContactView* findContact(u32 trackId) const;
    // Position of a body or ship in this snapshot.
    [[nodiscard]] bool positionOf(EntityId id, Vec3d& out) const;
};

class SnapshotBuilder {
public:
    static constexpr u32 kOrbitPathPoints = 160;

    // Captures the simulation at its current time. Ship positions are extrapolated from the last flight step
    // with their velocity, so motion stays smooth between coarse steps; bodies are exact (analytic orbits).
    void build(const Simulation& simulation, const Sandbox& sandbox, SystemSnapshot& out);

private:
    const std::vector<Vec3d>& orbitPath(EntityId body, const OrbitalElements& orbit, f64 parentGm);

    std::unordered_map<u64, std::vector<Vec3d>> m_orbitPaths; // orbits are static: sampled once per body
};

} // namespace gx
