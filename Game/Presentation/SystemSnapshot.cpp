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

const MarketView* SystemSnapshot::findMarket(EntityId port) const {
    const auto it =
        std::find_if(markets.begin(), markets.end(), [port](const MarketView& m) { return m.port == port; });
    return it == markets.end() ? nullptr : &*it;
}

const KnownPricesView* SystemSnapshot::findKnownPrices(EntityId port) const {
    const auto it = std::find_if(knownPrices.begin(), knownPrices.end(),
                                 [port](const KnownPricesView& k) { return k.port == port; });
    return it == knownPrices.end() ? nullptr : &*it;
}

const FleetShipView* SystemSnapshot::findFleetShip(EntityId id) const {
    const auto it =
        std::find_if(fleet.begin(), fleet.end(), [id](const FleetShipView& s) { return s.id == id; });
    return it == fleet.end() ? nullptr : &*it;
}

const DepositView* SystemSnapshot::findDeposit(EntityId field) const {
    const auto it = std::find_if(deposits.begin(), deposits.end(),
                                 [field](const DepositView& d) { return d.field == field; });
    return it == deposits.end() ? nullptr : &*it;
}

const WreckView* SystemSnapshot::findWreck(EntityId id) const {
    const auto it =
        std::find_if(wrecks.begin(), wrecks.end(), [id](const WreckView& w) { return w.id == id; });
    return it == wrecks.end() ? nullptr : &*it;
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
    if (const WreckView* wreck = findWreck(id)) {
        out = wreck->position;
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
    out.playerCredits = 0;
    out.playerReputation = sandbox.reputation();
    out.playerHostile = sandbox.hostile();
    out.treasury = sandbox.treasury();
    out.playerCargoCapacity = 0;
    out.playerCargo.clear();
    out.dockedPort = {};
    out.markets.clear();
    out.knownPrices.clear();
    out.fleet.clear();
    out.deposits.clear();
    out.wrecks.clear();
    out.debris.clear();
    out.playerCanMine = false;
    out.playerMining = false;
    out.playerMiningField = {};
    out.playerMiningRate = 0.0;
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

    // Economy: the company's account, the player's hold, every market (live) and what the player knows of
    // each.
    out.playerCredits = sandbox.company().account.credits;
    const CompanyBooks& books = sandbox.company();
    out.company = {books.account.credits,
                   books.debt,
                   books.instalment,
                   sandbox.creditLimit(world),
                   sandbox.fleetValue(world),
                   sandbox.companyCargoValue(world),
                   sandbox.companyWorth(world),
                   sandbox.companyPremiumPerHour(content::kHaulerHullPrice),
                   books.overdrawn,
                   sandbox.config().finance,
                   books.totals};
    const ComponentStore<FleetBrain>& brains = world.components<FleetBrain>();
    for (usize i = 0; i < brains.size(); ++i) {
        const EntityId id = brains.entities()[i];
        const FleetBrain& brain = brains.values()[i];
        const ShipIdentity* identity = world.components<ShipIdentity>().tryGet(id);
        if (identity == nullptr) {
            continue;
        }
        FleetShipView view;
        view.id = id;
        view.name = identity->name;
        view.shipClass = identity->shipClass;
        view.order = brain.order;
        view.task = brain.task;
        view.site = brain.site;
        view.market = brain.market;
        if (const OwnedShip* owned = world.components<OwnedShip>().tryGet(id)) {
            view.hullValue = owned->hullValue;
            view.insured = owned->insured;
            view.income = owned->income;
            view.expenses = owned->expenses;
        }
        if (const CargoHold* hold = world.components<CargoHold>().tryGet(id)) {
            view.cargoUsed = hold->used();
            view.cargoCapacity = hold->capacity;
        }
        if (const ShipModules* modules = world.components<ShipModules>().tryGet(id)) {
            const ShipModule* structure = structureOf(*modules);
            view.structure = structure != nullptr ? structure->fraction() : 1.0;
            view.powered = hasPower(*modules);
        }
        view.docked = sandbox.dockedPort(world, id);
        if (const MiningControl* mining = world.components<MiningControl>().tryGet(id)) {
            view.canMine = true;
            view.mining = mining->active;
        }
        view.armed = world.components<CombatControl>().contains(id);
        out.fleet.push_back(view);
    }
    const ComponentStore<Wreck>& wrecks = world.components<Wreck>();
    for (usize i = 0; i < wrecks.size(); ++i) {
        const EntityId id = wrecks.entities()[i];
        const Wreck& wreck = wrecks.values()[i];
        const Kinematics* state = kinematics.tryGet(id);
        const CargoHold* hold = world.components<CargoHold>().tryGet(id);
        if (state == nullptr || hold == nullptr) {
            continue;
        }
        out.wrecks.push_back({id, wreck.name, wreck.shipClass,
                              state->position + state->velocity * sinceFlight, state->velocity, hold->used(),
                              hold->items, (wreck.expires - now).toSeconds(), wreck.hulk,
                              wreck.knownTo(content::kFactionPlayer)});
    }
    for (const DebrisCloud& cloud : sandbox.debris()) {
        out.debris.push_back({cloud.centerAt(now), debrisRadius(cloud, now), debrisDensity(cloud, now)});
    }
    const ComponentStore<Deposit>& deposits = world.components<Deposit>();
    for (usize i = 0; i < deposits.size(); ++i) {
        const Deposit& deposit = deposits.values()[i];
        out.deposits.push_back({deposits.entities()[i], deposit.good, deposit.reserve, deposit.size});
    }
    if (out.playerAlive) {
        const EntityId player = sandbox.playerShip();
        if (const MiningControl* mining = world.components<MiningControl>().tryGet(player)) {
            out.playerCanMine = true;
            out.playerMining = mining->active;
            out.playerMiningField = mining->field;
            out.playerMiningRate = miningRate(world.components<ShipModules>().get(player));
        }
    }
    if (out.playerAlive) {
        const EntityId player = sandbox.playerShip();
        if (const CargoHold* hold = world.components<CargoHold>().tryGet(player)) {
            out.playerCargoCapacity = hold->capacity;
            out.playerCargo = hold->items;
        }
        out.dockedPort = sandbox.dockedPort(world, player);
    }
    const std::vector<GoodDef>& goods = sandbox.economy().goods();
    const ComponentStore<Market>& markets = world.components<Market>();
    for (usize i = 0; i < markets.size(); ++i) {
        const Market& market = markets.values()[i];
        MarketView view{markets.entities()[i], {}};
        for (const MarketGood& good : market.goods) {
            const f64 base = sandbox.economy().basePrice(good.good);
            view.rows.push_back(
                {good.good, good.good < goods.size() ? std::string_view(goods[good.good].name) : "?", base,
                 good.stock, good.target, buyPrice(good, base), sellPrice(good, base),
                 productionRate(market, good.good), consumptionRate(market, good.good), good.shortage});
        }
        out.markets.push_back(std::move(view));
    }
    for (const PortPrices& known : sandbox.playerPrices().ports) {
        out.knownPrices.push_back({known.port, (now - known.observed).toSeconds(), &known});
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
