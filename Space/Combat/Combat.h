#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/Kernel/SystemScheduler.h"
#include "Simulation/World/EntityRegistry.h"
#include "Space/Ships/Modules.h"

#include <span>
#include <string>
#include <vector>

// Combat (prompt §12, ADR-025). Weapons fire at sensor tracks, never at entities: the shooter's faction must
// hold a contact, and the shooter itself needs a fire-control lock on it (its own sensors, SNR >= 1). The
// aim point carries the lock's measurement error plus the weapon's pointing error, so quiet targets far
// away are hard to hit, radar makes fire control precise, and ghosts or lost tracks cannot be fired at.
//
//   Beam       hitscan; hits when the aim error (measurement, pointing and the target's acceleration during
//   the
//              fire-control latency) is smaller than the target's hull radius; damage per second
//   Projectile flies in a straight line from a lead solution that assumes the target keeps its velocity, so
//              accelerating targets dodge at long range; hits whatever hull it passes close enough to
//
// Hits go through applyDamage (module damage, reactor breaches). Combat runs on the flight cadence right
// after the flight system (ship positions refer to the same instant) and destroyed ships are removed in the
// EventResolution phase, after every subscriber has seen the ShipDestroyed event.
namespace gx {

class BinaryReader;
class BinaryWriter;
class SensorSystem;
class Simulation;
class World;
struct TickContext;

enum class WeaponKind : u8 { Beam, Projectile, Count };

[[nodiscard]] const char* toString(WeaponKind kind);

struct WeaponDef {
    std::string name;
    WeaponKind kind = WeaponKind::Beam;
    f64 range = 0.0;           // m: fire control does not shoot beyond it
    f64 damage = 0.0;          // Beam: per second on target; Projectile: per hit
    f64 cooldown = 0.0;        // s between shots (projectiles)
    f64 projectileSpeed = 0.0; // m/s relative to the shooter
    f64 pointingError = 0.0;   // rad, 1-sigma
};

// Fire orders of a ship: the sensor track it fires at (0: hold fire).
struct CombatControl {
    u32 targetTrack = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("targetTrack", targetTrack);
    }
};

struct Projectile {
    Vec3d position;
    Vec3d velocity;
    EntityId shooter;
    u32 weapon = 0;
    f64 remaining = 0.0; // s of flight left

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("position", position);
        ar.io("velocity", velocity);
        ar.io("shooter", shooter);
        ar.io("weapon", weapon);
        ar.io("remaining", remaining);
    }
};

// Event: a ship was hit.
struct ShipDamaged {
    EntityId ship;
    EntityId attacker;
    f64 damage = 0.0;
    ModuleType module = ModuleType::Structure;
    bool moduleDestroyed = false;
};

// Event: a ship was destroyed. The entity is still alive while subscribers run and is removed afterwards.
struct ShipDestroyed {
    EntityId ship;
    EntityId attacker;
    Vec3d position;
    bool reactorBreach = false;
};

// Presentation only (not simulation state): recent beam shots and explosions.
struct BeamShot {
    SimTime time;
    EntityId shooter;
    Vec3d from;
    Vec3d to;
    bool hit = false;
};

struct Explosion {
    SimTime time;
    Vec3d position;
};

struct CombatStats {
    u64 shotsFired = 0;
    u64 hits = 0;
    u64 shipsDestroyed = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("shotsFired", shotsFired);
        ar.io("hits", hits);
        ar.io("shipsDestroyed", shipsDestroyed);
    }
};

// State of one weapon's fire control, as the owner's UI can know it.
struct FireSolution {
    bool hasTarget = false;
    bool inRange = false;
    bool locked = false;
    f64 distance = 0.0; // to the track estimate
};

// Earliest time t >= 0 at which a projectile of speed `speed` fired now from the origin meets a target at
// `relativePosition` moving at `relativeVelocity`; negative if it cannot catch it.
[[nodiscard]] f64 interceptTime(const Vec3d& relativePosition, const Vec3d& relativeVelocity, f64 speed);

// Closest approach during [0, dt] of a point starting at `relativePosition` and moving at `relativeVelocity`
// with respect to a target at the origin: returns the time of closest approach.
[[nodiscard]] f64 closestApproachTime(const Vec3d& relativePosition, const Vec3d& relativeVelocity, f64 dt);

class CombatSystem {
public:
    static void registerTypes(Simulation& simulation);

    // Weapon table (content). Required before install; weapon modules index into it.
    void setWeapons(std::vector<WeaponDef> weapons) { m_weapons = std::move(weapons); }
    [[nodiscard]] const std::vector<WeaponDef>& weapons() const { return m_weapons; }

    // Registers "Space.Combat" (Simulation phase, after flight), "Space.Combat.Cleanup" (EventResolution) and
    // the "Space.Combat" state block. `period` must match the flight period; change both through setPeriod.
    void install(Simulation& simulation, const SensorSystem& sensors, SimDuration period);
    void setPeriod(Simulation& simulation, SimDuration period);

    void update(const TickContext& context);
    void cleanup(const TickContext& context);

    [[nodiscard]] const std::vector<Projectile>& projectiles() const { return m_projectiles; }
    [[nodiscard]] const std::vector<BeamShot>& recentBeams() const { return m_beams; }
    [[nodiscard]] const std::vector<Explosion>& recentExplosions() const { return m_explosions; }
    [[nodiscard]] const CombatStats& stats() const { return m_stats; }
    // Fire-control state of a ship's weapons towards its current target (for its owner's UI).
    [[nodiscard]] FireSolution fireSolution(const World& world, EntityId ship, u32 weapon) const;

    static constexpr SimDuration kBeamVisibleFor = SimDuration::milliseconds(300);
    static constexpr SimDuration kExplosionVisibleFor = SimDuration::seconds(4);

private:
    struct Lock {
        bool valid = false;
        Vec3d position; // measured
        Vec3d velocity;
        Vec3d acceleration; // true, unknown to fire control: it only turns into a miss distance
        f64 sigma = 0.0;
        EntityId target;
    };

    [[nodiscard]] Lock acquireLock(const World& world, EntityId shooter, u32 faction, u32 track) const;
    void hit(const TickContext& context, EntityId target, EntityId attacker, f64 damage, u64 salt);
    void writeState(BinaryWriter& writer) const;
    void readState(BinaryReader& reader);

    const SensorSystem* m_sensors = nullptr;
    std::vector<WeaponDef> m_weapons;
    SystemId m_system = kInvalidSystemId;
    SystemId m_cleanupSystem = kInvalidSystemId;
    // Saved state.
    std::vector<Projectile> m_projectiles;
    CombatStats m_stats;
    // Per step.
    std::vector<EntityId> m_dying;
    // Presentation.
    std::vector<BeamShot> m_beams;
    std::vector<Explosion> m_explosions;
};

} // namespace gx
