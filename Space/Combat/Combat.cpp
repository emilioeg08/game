#include "Space/Combat/Combat.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Serialization/Binary.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Sensors/Sensors.h"
#include "Space/Ships/Ship.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gx {
namespace {

constexpr u64 kFireStream = fnv1a64("space.combat.fire");
constexpr u64 kDamageStream = fnv1a64("space.combat.damage");
constexpr f64 kLockSnr = 1.0;
// Fire control aims where the target was a moment ago, projected at constant velocity: whatever the target's
// acceleration adds during this latency is a miss distance (0.5 a t^2). A courier burning at full thrust
// (50 km/s^2) sits ~60 m off the aim point; a hauler (15 km/s^2) only ~20 m.
constexpr f64 kFireControlLatency = 0.05;      // s
constexpr f64 kProjectileLifetimeFactor = 1.5; // flight time allowed, relative to range / speed

u64 entityKey(EntityId entity) {
    return (static_cast<u64>(entity.generation) << 32) | entity.index;
}

bool inHyperspace(const World& world, EntityId ship) {
    const ShipControl* control = world.components<ShipControl>().tryGet(ship);
    return control != nullptr && control->phase == DrivePhase::Hyperspace;
}

} // namespace

const char* toString(WeaponKind kind) {
    switch (kind) {
    case WeaponKind::Beam:
        return "Beam";
    case WeaponKind::Projectile:
        return "Projectile";
    case WeaponKind::Count:
        break;
    }
    return "?";
}

f64 interceptTime(const Vec3d& relativePosition, const Vec3d& relativeVelocity, f64 speed) {
    const f64 a = dot(relativeVelocity, relativeVelocity) - speed * speed;
    const f64 b = 2.0 * dot(relativePosition, relativeVelocity);
    const f64 c = dot(relativePosition, relativePosition);
    if (std::abs(a) < 1e-9) {
        return b < 0.0 ? -c / b : -1.0;
    }
    const f64 discriminant = b * b - 4.0 * a * c;
    if (discriminant < 0.0) {
        return -1.0;
    }
    const f64 root = std::sqrt(discriminant);
    const f64 t1 = (-b - root) / (2.0 * a);
    const f64 t2 = (-b + root) / (2.0 * a);
    const f64 first = std::min(t1, t2);
    const f64 second = std::max(t1, t2);
    if (first >= 0.0) {
        return first;
    }
    return second >= 0.0 ? second : -1.0;
}

f64 closestApproachTime(const Vec3d& relativePosition, const Vec3d& relativeVelocity, f64 dt) {
    const f64 speedSquared = dot(relativeVelocity, relativeVelocity);
    if (speedSquared < 1e-12) {
        return 0.0;
    }
    return std::clamp(-dot(relativePosition, relativeVelocity) / speedSquared, 0.0, dt);
}

void CombatSystem::registerTypes(Simulation& simulation) {
    World& world = simulation.world();
    world.registerComponent<ShipModules>("Space.ShipModules");
    world.registerComponent<ShipDesignStats>("Space.ShipDesignStats");
    world.registerComponent<CombatControl>("Space.CombatControl");
    simulation.events().registerEvent<ShipDamaged>("Space.ShipDamaged");
    simulation.events().registerEvent<ShipDestroyed>("Space.ShipDestroyed");
}

void CombatSystem::install(Simulation& simulation, const SensorSystem& sensors, SimDuration period) {
    GX_CHECK(!m_weapons.empty(), "CombatSystem::install needs the weapon table");
    m_sensors = &sensors;
    simulation.addStateBlock(
        "Space.Combat", [this](BinaryWriter& writer) { writeState(writer); },
        [this](BinaryReader& reader) { readState(reader); });
    m_system = simulation.addSystem(
        {"Space.Combat", TickPhase::Simulation, period, {}, [this](const TickContext& c) { update(c); }});
    m_cleanupSystem = simulation.addSystem(
        {"Space.Combat.Cleanup", TickPhase::EventResolution, period, {}, [this](const TickContext& c) {
             cleanup(c);
         }});
}

void CombatSystem::setPeriod(Simulation& simulation, SimDuration period) {
    simulation.setSystemPeriod(m_system, period);
    simulation.setSystemPeriod(m_cleanupSystem, period);
}

CombatSystem::Lock CombatSystem::acquireLock(const World& world, EntityId shooter, u32 faction,
                                             u32 track) const {
    Lock lock;
    const SensorContact* contact = m_sensors->findContact(faction, track);
    if (contact == nullptr || contact->ghost || !world.isAlive(contact->target)) {
        return lock; // nothing there to lock on
    }
    const EntityId target = contact->target;
    const Kinematics* targetState = world.components<Kinematics>().tryGet(target);
    const Kinematics* shooterState = world.components<Kinematics>().tryGet(shooter);
    const SensorSuite* suite = world.components<SensorSuite>().tryGet(shooter);
    const SignatureProfile* profile = world.components<SignatureProfile>().tryGet(target);
    const ShipDrive* drive = world.components<ShipDrive>().tryGet(target);
    const ShipControl* control = world.components<ShipControl>().tryGet(target);
    if (targetState == nullptr || shooterState == nullptr || suite == nullptr || profile == nullptr ||
        drive == nullptr || control == nullptr || control->phase == DrivePhase::Hyperspace ||
        inHyperspace(world, shooter)) {
        return lock;
    }
    static const SensorSuite kNoSensors{};
    const SensorSuite* targetSuite = world.components<SensorSuite>().tryGet(target);
    const f64 emission = shipEmission(*profile, *drive, *targetState, *control,
                                      targetSuite != nullptr ? *targetSuite : kNoSensors);
    const f64 distance = length(targetState->position - shooterState->position);
    const f64 passive = passiveSnr(emission, suite->passiveSensitivity, distance);
    const f64 active = suite->activeOn && suite->activeStrength > 0.0
                           ? activeSnr(suite->activeStrength, profile->crossSection, distance)
                           : 0.0;
    if (std::max(passive, active) < kLockSnr) {
        return lock;
    }
    lock.valid = true;
    lock.position = targetState->position;
    lock.velocity = targetState->velocity;
    lock.acceleration = targetState->acceleration;
    lock.sigma = measurementSigma(distance, passive, active);
    lock.target = target;
    return lock;
}

FireSolution CombatSystem::fireSolution(const World& world, EntityId ship, u32 weapon) const {
    FireSolution solution;
    const CombatControl* orders = world.components<CombatControl>().tryGet(ship);
    const ShipModules* modules = world.components<ShipModules>().tryGet(ship);
    const ShipIdentity* identity = world.components<ShipIdentity>().tryGet(ship);
    const Kinematics* state = world.components<Kinematics>().tryGet(ship);
    if (orders == nullptr || orders->targetTrack == 0 || modules == nullptr || identity == nullptr ||
        state == nullptr || weapon >= modules->modules.size() || m_sensors == nullptr) {
        return solution;
    }
    const SensorContact* contact = m_sensors->findContact(identity->faction, orders->targetTrack);
    if (contact == nullptr) {
        return solution;
    }
    solution.hasTarget = true;
    solution.distance = length(contact->position - state->position);
    const ShipModule& module = modules->modules[weapon];
    if (module.type == ModuleType::Weapon && module.weapon < m_weapons.size()) {
        solution.inRange = solution.distance <= m_weapons[module.weapon].range;
    }
    solution.locked = acquireLock(world, ship, identity->faction, orders->targetTrack).valid;
    return solution;
}

void CombatSystem::hit(const TickContext& context, EntityId target, EntityId attacker, f64 damage, u64 salt) {
    World& world = context.world;
    ShipModules* modules = world.components<ShipModules>().tryGet(target);
    if (modules == nullptr || std::find(m_dying.begin(), m_dying.end(), target) != m_dying.end()) {
        return;
    }
    Rng rng = Rng::forStream(context.worldSeed, hashCombine(kDamageStream, entityKey(target)),
                             hashCombine(context.runIndex, salt));
    const DamageResult result = applyDamage(*modules, damage, rng);
    modules->lastDamaged = context.now;
    ++m_stats.hits;
    applyModuleEffects(world, target);
    context.events.channel<ShipDamaged>().emit(
        ShipDamaged{target, attacker, damage, result.moduleHit, result.moduleDestroyed});
    if (result.shipDestroyed) {
        m_dying.push_back(target);
        ++m_stats.shipsDestroyed;
        const Vec3d position = world.components<Kinematics>().get(target).position;
        m_explosions.push_back({context.now, position});
        context.events.channel<ShipDestroyed>().emit(
            ShipDestroyed{target, attacker, position, result.reactorBreach});
    }
}

void CombatSystem::update(const TickContext& context) {
    World& world = context.world;
    const f64 dt = context.dt.toSeconds();
    std::erase_if(m_beams, [&](const BeamShot& shot) { return context.now - shot.time > kBeamVisibleFor; });
    std::erase_if(m_explosions, [&](const Explosion& explosion) {
        return context.now - explosion.time > kExplosionVisibleFor;
    });
    if (dt <= 0.0) {
        return;
    }
    const ComponentStore<Kinematics>& kinematics = world.components<Kinematics>();
    const ComponentStore<ShipDesignStats>& designs = world.components<ShipDesignStats>();
    u64 hitSalt = 0;

    // Projectiles in flight: closest approach to every hull during the step, both moving in straight lines
    // (the flight step that just ran integrates positions with the end-of-step velocity, so a ship's
    // position one step ago is exactly position - velocity * dt).
    {
        GX_PROFILE_SCOPE("Space.Combat.Projectiles");
        usize kept = 0;
        for (usize p = 0; p < m_projectiles.size(); ++p) {
            Projectile projectile = m_projectiles[p];
            const f64 span = std::min(dt, projectile.remaining);
            EntityId struck;
            f64 earliest = std::numeric_limits<f64>::infinity();
            for (usize s = 0; s < designs.size(); ++s) {
                const EntityId ship = designs.entities()[s];
                const Kinematics* state = kinematics.tryGet(ship);
                if (ship == projectile.shooter || state == nullptr || inHyperspace(world, ship)) {
                    continue;
                }
                const f64 hull = designs.values()[s].hullRadius;
                const Vec3d start = projectile.position - (state->position - state->velocity * dt);
                const Vec3d relativeVelocity = projectile.velocity - state->velocity;
                const f64 reach = length(relativeVelocity) * span + hull;
                if (lengthSquared(start) > reach * reach) {
                    continue; // cannot come close enough during this step
                }
                const f64 t = closestApproachTime(start, relativeVelocity, span);
                if (lengthSquared(start + relativeVelocity * t) < hull * hull && t < earliest) {
                    earliest = t;
                    struck = ship;
                }
            }
            if (struck.isValid()) {
                const f64 damage =
                    projectile.weapon < m_weapons.size() ? m_weapons[projectile.weapon].damage : 0.0;
                hit(context, struck, projectile.shooter, damage, hitSalt++);
                continue;
            }
            projectile.position += projectile.velocity * span;
            projectile.remaining -= dt;
            if (projectile.remaining > 0.0) {
                m_projectiles[kept++] = projectile;
            }
        }
        m_projectiles.resize(kept);
    }

    // Weapons: every ship with fire orders shoots at its track if its fire control holds a lock.
    GX_PROFILE_SCOPE("Space.Combat.Weapons");
    ComponentStore<CombatControl>& orders = world.components<CombatControl>();
    ComponentStore<ShipModules>& moduleStore = world.components<ShipModules>();
    for (usize i = 0; i < orders.size(); ++i) {
        const EntityId ship = orders.entities()[i];
        ShipModules* modules = moduleStore.tryGet(ship);
        if (modules == nullptr) {
            continue;
        }
        for (ShipModule& module : modules->modules) {
            module.cooldown = std::max(0.0, module.cooldown - dt);
        }
        const u32 track = orders.values()[i].targetTrack;
        const ShipIdentity* identity = world.components<ShipIdentity>().tryGet(ship);
        const Kinematics* shooter = kinematics.tryGet(ship);
        if (track == 0 || identity == nullptr || shooter == nullptr || !hasPower(*modules) ||
            std::find(m_dying.begin(), m_dying.end(), ship) != m_dying.end()) {
            continue;
        }
        const Lock lock = acquireLock(world, ship, identity->faction, track);
        if (!lock.valid || std::find(m_dying.begin(), m_dying.end(), lock.target) != m_dying.end()) {
            continue;
        }
        const ShipDesignStats* targetDesign = designs.tryGet(lock.target);
        const f64 hull = targetDesign != nullptr ? targetDesign->hullRadius : 0.0;
        const f64 distance = length(lock.position - shooter->position);
        Rng rng =
            Rng::forStream(context.worldSeed, hashCombine(kFireStream, entityKey(ship)), context.runIndex);
        for (ShipModule& module : modules->modules) {
            if (module.type != ModuleType::Weapon || !module.functional() ||
                module.weapon >= m_weapons.size()) {
                continue;
            }
            const WeaponDef& weapon = m_weapons[module.weapon];
            if (distance > weapon.range) {
                continue;
            }
            if (weapon.kind == WeaponKind::Beam) {
                const f64 pointing = distance * weapon.pointingError;
                const f64 sigma = std::sqrt(lock.sigma * lock.sigma + pointing * pointing);
                const Vec3d error = sensorNoise(rng, sigma) +
                                    lock.acceleration * (0.5 * kFireControlLatency * kFireControlLatency);
                const bool struck = lengthSquared(error) < hull * hull;
                ++m_stats.shotsFired;
                m_beams.push_back({context.now, ship, shooter->position, lock.position + error, struck});
                if (struck) {
                    hit(context, lock.target, ship, weapon.damage * dt, hitSalt++);
                }
            } else if (module.cooldown <= 0.0 && weapon.projectileSpeed > 0.0) {
                // Lead solution on the measured track, assuming the target keeps its velocity.
                const Vec3d measuredPosition = lock.position + sensorNoise(rng, lock.sigma);
                const Vec3d measuredVelocity = lock.velocity + sensorNoise(rng, lock.sigma / 10.0);
                const Vec3d offset = measuredPosition - shooter->position;
                const Vec3d closing = measuredVelocity - shooter->velocity;
                const f64 t = interceptTime(offset, closing, weapon.projectileSpeed);
                if (t < 0.0) {
                    continue;
                }
                Vec3d direction = offset + closing * t;
                direction = direction / std::max(length(direction), 1e-9);
                direction = direction + sensorNoise(rng, weapon.pointingError);
                direction = direction / std::max(length(direction), 1e-9);
                m_projectiles.push_back(
                    {shooter->position, shooter->velocity + direction * weapon.projectileSpeed, ship,
                     module.weapon, weapon.range / weapon.projectileSpeed * kProjectileLifetimeFactor});
                module.cooldown = weapon.cooldown;
                ++m_stats.shotsFired;
            }
        }
    }
}

void CombatSystem::cleanup(const TickContext& context) {
    for (const EntityId ship : m_dying) {
        if (context.world.isAlive(ship)) {
            context.world.destroyEntity(ship);
        }
    }
    m_dying.clear();
}

void CombatSystem::writeState(BinaryWriter& writer) const {
    writer.io(m_projectiles);
    writer.io(m_stats);
}

void CombatSystem::readState(BinaryReader& reader) {
    std::vector<Projectile> projectiles;
    CombatStats stats;
    reader.io(projectiles);
    reader.io(stats);
    if (reader.ok()) {
        m_projectiles = std::move(projectiles);
        m_stats = stats;
        m_beams.clear();
        m_explosions.clear();
        m_dying.clear();
    }
}

} // namespace gx
