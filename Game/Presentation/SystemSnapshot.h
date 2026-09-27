#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/Economy/Economy.h"
#include "Simulation/World/EntityRegistry.h"
#include "Space/Bodies/CelestialBody.h"
#include "Space/Combat/Combat.h"
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
    u32 track = 0;       // Pursue: the sensor track followed
    u32 fireTrack = 0;   // the sensor track its weapons fire at (0: holding fire)
    f64 structure = 1.0; // hull integrity share
    bool powered = true; // false: reactor out, drifting
};

// One module of the player's ship (the damage panel).
struct ModuleView {
    ModuleType type = ModuleType::Structure;
    u32 weapon = 0;
    f64 fraction = 1.0;
    bool functional = true;
    f64 cooldown = 0.0;
    FireSolution fire; // weapons only
};

// Weapon fire and explosions. `visible`: close enough to the player to be seen (anything else is debug only).
struct BeamView {
    Vec3d from;
    Vec3d to;
    bool hit = false;
    bool byPlayer = false;
    bool visible = false;
};

struct ProjectileView {
    Vec3d position;
    Vec3d velocity;
    bool byPlayer = false;
    bool visible = false;
};

struct ExplosionView {
    Vec3d position;
    f64 ageSeconds = 0.0;
    bool visible = false;
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

// One good of a port's live market.
struct MarketRowView {
    GoodId good = 0;
    std::string_view name;
    f64 basePrice = 0.0;
    f64 stock = 0.0;
    f64 target = 0.0;
    i64 buy = 0;          // the port sells at (next tonne)
    i64 sell = 0;         // the port buys at
    f64 production = 0.0; // t/h at full efficiency
    f64 consumption = 0.0;
    f64 shortage = 0.0; // cumulative unmet demand
};

// Live market of a port. The player sees the one it is docked at; the rest is for the economy inspector.
struct MarketView {
    EntityId port;
    std::vector<MarketRowView> rows;
};

// Prices the player knows for a port, and how old they are.
struct KnownPricesView {
    EntityId port;
    f64 ageSeconds = 0.0;
    const PortPrices* prices = nullptr; // valid until the next simulation step
};

// Read-only picture of the simulation for rendering, UI and debug tools: the presentation layer never touches
// live components. Views reference names owned by the World and are valid until the next simulation step.
struct SystemSnapshot {
    SimTime time;
    std::vector<BodyView> bodies;
    std::vector<ShipView> ships;       // ground truth: the client shows only its own fleet unless debugging
    std::vector<ContactView> contacts; // the player's sensor picture
    std::vector<BeamView> beams;
    std::vector<ProjectileView> projectiles;
    std::vector<ExplosionView> explosions;
    u32 playerFaction = 0;
    bool playerAlive = false;
    f64 playerRespawnIn = 0.0; // s, while the player has no ship
    SensorSuite playerSensors; // the player's ship: radar/transponder switches
    f64 playerEmission = 0.0;  // how bright the player's ship is right now
    std::vector<ModuleView> playerModules;
    bool tactical = false; // fine flight/combat steps
    // Economy.
    i64 playerCredits = 0;
    u32 playerCargoCapacity = 0;
    std::vector<CargoItem> playerCargo;
    EntityId dockedPort; // the port whose market the player can trade at, or invalid
    std::vector<MarketView> markets;
    std::vector<KnownPricesView> knownPrices;

    [[nodiscard]] const BodyView* findBody(EntityId id) const;
    [[nodiscard]] const ShipView* findShip(EntityId id) const;
    [[nodiscard]] const ContactView* findContact(u32 trackId) const;
    [[nodiscard]] const MarketView* findMarket(EntityId port) const;
    [[nodiscard]] const KnownPricesView* findKnownPrices(EntityId port) const;
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
