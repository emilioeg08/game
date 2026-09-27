#include "Game/Presentation/SystemSnapshot.h"

#include "Game/Sandbox/Content.h"
#include "Game/Sandbox/Sandbox.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Ships/Flight.h"

#include <algorithm>

namespace gx {
namespace {

// Weapon fire is seen within this distance of the player's ship; explosions much farther.
constexpr f64 kFireVisibleRange = 1e7;
constexpr f64 kExplosionVisibleRange = 5e9;

} // namespace

const BodyView* SystemSnapshot::findBody(EntityId id) const {
    const auto it =
        std::find_if(bodies.begin(), bodies.end(), [id](const BodyView& b) { return b.id == id; });
    return it == bodies.end() ? nullptr : &*it;
}

const ShipView* SystemSnapshot::findShip(EntityId id) const {
    const auto it = std::find_if(ships.begin(), ships.end(), [id](const ShipView& s) { return s.id == id; });
    return it == ships.end() ? nullptr : &*it;
}

const ContactView* SystemSnapshot::findContact(u32 trackId) const {
    const auto it = std::find_if(contacts.begin(), contacts.end(),
                                 [trackId](const ContactView& c) { return c.trackId == trackId; });
    return it == contacts.end() ? nullptr : &*it;
}

bool SystemSnapshot::positionOf(EntityId id, Vec3d& out) const {
    if (const ShipView* ship = findShip(id)) {
        out = ship->position;
        return true;
    }
    if (const BodyView* body = findBody(id)) {
        out = body->position;
        return true;
    }
    return false;
}

const std::vector<Vec3d>& SnapshotBuilder::orbitPath(EntityId body, const OrbitalElements& orbit,
                                                     f64 parentGm) {
    const u64 key = (static_cast<u64>(body.generation) << 32) | body.index;
    auto [it, inserted] = m_orbitPaths.try_emplace(key);
    if (inserted) {
        it->second.reserve(kOrbitPathPoints);
        for (u32 i = 0; i < kOrbitPathPoints; ++i) {
            const f64 eccentricAnomaly = kTwoPi * static_cast<f64>(i) / kOrbitPathPoints;
            it->second.push_back(orbitStateAtEccentricAnomaly(orbit, parentGm, eccentricAnomaly).position);
        }
    }
    return it->second;
}

void SnapshotBuilder::build(const Simulation& simulation, const Sandbox& sandbox, SystemSnapshot& out) {
    const World& world = simulation.world();
    const SimTime now = simulation.now();
    out.time = now;
    out.bodies.clear();
    out.ships.clear();
    out.contacts.clear();
    out.beams.clear();
    out.projectiles.clear();
    out.explosions.clear();
    out.playerModules.clear();
    out.playerFaction = content::kFactionPlayer;
    out.playerAlive = false;
    out.playerRespawnIn = 0.0;
    out.playerSensors = {};
    out.playerEmission = 0.0;
    out.tactical = sandbox.tactical();

    const ComponentStore<CelestialBody>& bodies = world.components<CelestialBody>();
    const ComponentStore<OrbitsParent>& orbits = world.components<OrbitsParent>();
    for (usize i = 0; i < bodies.size(); ++i) {
        const EntityId id = bodies.entities()[i];
        const CelestialBody& body = bodies.values()[i];
        BodyView view;
        view.id = id;
        view.name = body.name;
        view.kind = body.kind;
        view.radius = body.radius;
        view.wellRadius = gravityWellRadius(body);
        view.position = bodyStateAt(world, id, now).position;
        if (const OrbitsParent* link = orbits.tryGet(id)) {
            view.parent = link->parent;
            view.parentPosition = bodyStateAt(world, link->parent, now).position;
            view.orbitPath = &orbitPath(id, link->orbit, bodies.get(link->parent).gm);
        }
        out.bodies.push_back(view);
    }

    // Ships were last integrated when the flight system last ran; extrapolate to "now" for display only.
    const f64 sinceFlight = (now - simulation.scheduler().system(sandbox.flightSystem()).lastRun).toSeconds();
    const ComponentStore<ShipIdentity>& identities = world.components<ShipIdentity>();
    const ComponentStore<Kinematics>& kinematics = world.components<Kinematics>();
    const ComponentStore<ShipControl>& controls = world.components<ShipControl>();
    for (usize i = 0; i < identities.size(); ++i) {
        const EntityId id = identities.entities()[i];
        const ShipIdentity& identity = identities.values()[i];
        const Kinematics* state = kinematics.tryGet(id);
        const ShipControl* control = controls.tryGet(id);
        if (state == nullptr || control == nullptr) {
            continue;
        }
        ShipView view;
        view.id = id;
        view.name = identity.name;
        view.faction = identity.faction;
        view.shipClass = identity.shipClass;
        view.position = state->position + state->velocity * sinceFlight;
        view.velocity = state->velocity;
        view.acceleration = state->acceleration;
        view.mode = control->mode;
        view.phase = control->phase;
        view.chargeRemaining = control->chargeRemaining;
        view.target = control->target;
        view.arrived = control->arrived;
        view.isPlayer = id == sandbox.playerShip();
        view.track = control->mode == FlightMode::Pursue ? control->track : 0;
        if (const CombatControl* orders = world.components<CombatControl>().tryGet(id)) {
            view.fireTrack = orders->targetTrack;
        }
        if (const ShipModules* modules = world.components<ShipModules>().tryGet(id)) {
            const ShipModule* structure = structureOf(*modules);
            view.structure = structure != nullptr ? structure->fraction() : 1.0;
            view.powered = hasPower(*modules);
        }
        const TargetState target = resolveTarget(world, *control, now);
        view.targetPosition = target.valid ? target.position : view.position;
        out.ships.push_back(view);
        if (view.isPlayer) {
            out.playerAlive = true;
            if (const ShipModules* modules = world.components<ShipModules>().tryGet(id)) {
                for (u32 m = 0; m < modules->modules.size(); ++m) {
                    const ShipModule& module = modules->modules[m];
                    ModuleView moduleView{module.type,         module.weapon,   module.fraction(),
                                          module.functional(), module.cooldown, {}};
                    if (module.type == ModuleType::Weapon) {
                        moduleView.fire = sandbox.combat().fireSolution(world, id, m);
                    }
                    out.playerModules.push_back(moduleView);
                }
            }
            const SensorSuite* suite = world.components<SensorSuite>().tryGet(id);
            const SignatureProfile* profile = world.components<SignatureProfile>().tryGet(id);
            const ShipDrive* drive = world.components<ShipDrive>().tryGet(id);
            if (suite != nullptr && profile != nullptr && drive != nullptr) {
                out.playerSensors = *suite;
                out.playerEmission = shipEmission(*profile, *drive, *state, *control, *suite);
            }
        }
    }

    if (!out.playerAlive) {
        out.playerRespawnIn = std::max(0.0, (sandbox.playerRespawnAt() - now).toSeconds());
    }

    // Weapon fire and explosions near the player (all of them are kept, flagged, for the debug view).
    Vec3d player;
    const bool hasPlayer = out.playerAlive && out.positionOf(sandbox.playerShip(), player);
    const auto near = [&](const Vec3d& position, f64 range) {
        return hasPlayer && lengthSquared(position - player) < range * range;
    };
    const CombatSystem& combat = sandbox.combat();
    const std::vector<BeamShot>& shots = combat.recentBeams();
    for (usize i = 0; i < shots.size(); ++i) {
        const BeamShot& shot = shots[i];
        // Only each shooter's latest volley: older beams of a moving shooter would smear into a band.
        const bool superseded = std::any_of(
            shots.begin() + static_cast<std::ptrdiff_t>(i) + 1, shots.end(),
            [&](const BeamShot& later) { return later.shooter == shot.shooter && later.time > shot.time; });
        if (superseded) {
            continue;
        }
        const bool byPlayer = shot.shooter == sandbox.playerShip();
        out.beams.push_back(
            {shot.from, shot.to, shot.hit, byPlayer,
             byPlayer || near(shot.from, kFireVisibleRange) || near(shot.to, kFireVisibleRange)});
    }
    for (const Projectile& projectile : combat.projectiles()) {
        const bool byPlayer = projectile.shooter == sandbox.playerShip();
        const Vec3d position = projectile.position + projectile.velocity * sinceFlight;
        out.projectiles.push_back(
            {position, projectile.velocity, byPlayer, byPlayer || near(position, kFireVisibleRange)});
    }
    for (const Explosion& explosion : combat.recentExplosions()) {
        out.explosions.push_back({explosion.position, (now - explosion.time).toSeconds(),
                                  near(explosion.position, kExplosionVisibleRange)});
    }

    for (const SensorContact& contact : sandbox.sensors().picture(out.playerFaction).contacts) {
        ContactView view;
        view.trackId = contact.trackId;
        view.level = contact.level;
        view.ageSeconds = (now - contact.lastSeen).toSeconds();
        view.position = contact.position + contact.velocity * view.ageSeconds;
        view.velocity = contact.velocity;
        view.uncertainty = contact.uncertainty;
        view.shipClass = contact.shipClass;
        view.faction = contact.faction;
        view.ghost = contact.ghost;
        if (contact.level == ContactLevel::Identified) {
            if (const ShipIdentity* identity = identities.tryGet(contact.target)) {
                view.name = identity->name;
            }
        }
        out.contacts.push_back(view);
    }
}

} // namespace gx
