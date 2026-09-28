// Wrecks, debris and salvage (ADR-039). A destroyed ship breaks up along its modules (breakUp): the hulk and
// a few fragments drift away with what survived of the cargo and the scrap of the modules still in one piece,
// and the explosion leaves a cloud of debris that hurts whoever crosses it fast. Anybody with a hold can
// salvage a wreck its faction knows about; what nobody takes drifts out of reach and is lost.
#include "Game/Sandbox/Content.h"
#include "Game/Sandbox/Sandbox.h"
#include "Game/Sandbox/SandboxDetail.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Bodies/CelestialBody.h"
#include "Space/Ships/Modules.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace gx {
namespace {

using sandbox_detail::entityKey;

constexpr u64 kWreckStream = fnv1a64("sandbox.wreck.breakup");
constexpr u64 kDebrisStream = fnv1a64("sandbox.debris.impacts");

f64 designSize(u32 shipClass) {
    f64 size = 0.0;
    for (const content::ModuleDef& module : content::shipModules(shipClass)) {
        size += module.health;
    }
    return size;
}

} // namespace

void Sandbox::breakUpShip(World& world, const ShipDestroyed& event, SimTime now) {
    const ShipModules* modules = world.components<ShipModules>().tryGet(event.ship);
    const Kinematics* state = world.components<Kinematics>().tryGet(event.ship);
    const ShipIdentity* identity = world.components<ShipIdentity>().tryGet(event.ship);
    if (modules == nullptr || state == nullptr || identity == nullptr) {
        return;
    }
    // Copies: creating the wrecks adds to the component stores, which may move what they hold.
    const Kinematics kinematics = *state;
    const std::string name = identity->name;
    const u32 shipClass = identity->shipClass;
    const u32 faction = identity->faction;
    Rng rng = Rng::forStream(m_config.seed, hashCombine(kWreckStream, entityKey(event.ship)),
                             static_cast<u64>((now - SimTime::epoch()).count()));
    const std::vector<FragmentDesc> pieces = breakUp(*modules, kinematics.velocity, event.reactorBreach, rng);

    // What survives of the cargo is dealt out by the pieces' size; the rest is gone for good.
    std::vector<std::vector<CargoItem>> cargo(pieces.size());
    m_stats.cargoLost.resize(std::max<usize>(m_stats.cargoLost.size(), content::kGoodCount), 0);
    if (const CargoHold* hold = world.components<CargoHold>().tryGet(event.ship)) {
        const f64 survival = cargoSurvival(event.reactorBreach);
        for (const CargoItem& item : hold->items) {
            const auto survived = static_cast<u32>(std::floor(static_cast<f64>(item.tonnes) * survival));
            m_stats.cargoLost[item.good] += item.tonnes - survived;
            u32 dealt = 0;
            for (usize p = 1; p < pieces.size(); ++p) {
                const auto share = static_cast<u32>(std::floor(static_cast<f64>(survived) * pieces[p].share));
                if (share > 0) {
                    cargo[p].push_back({item.good, share});
                    dealt += share;
                }
            }
            if (survived > dealt) {
                cargo.front().push_back({item.good, survived - dealt}); // the rest stays in the hulk
            }
        }
    }

    // Who knows where it is: its own side, whoever shot it, and every faction that was tracking it.
    u32 known = 1u << faction;
    if (const ShipIdentity* attacker = world.isAlive(event.attacker)
                                           ? world.components<ShipIdentity>().tryGet(event.attacker)
                                           : nullptr) {
        known |= 1u << attacker->faction;
    }
    for (u32 watcher = 0; watcher < content::kFactionCount; ++watcher) {
        for (const SensorContact& contact : m_sensors.picture(watcher).contacts) {
            if (!contact.ghost && contact.target == event.ship) {
                known |= 1u << watcher;
            }
        }
    }

    u32 salvageable = 0;
    for (usize p = 0; p < pieces.size(); ++p) {
        CargoHold hold;
        for (const CargoItem& item : cargo[p]) {
            hold.capacity += item.tonnes;
            hold.add(item.good, item.tonnes);
        }
        const auto scrap = static_cast<u32>(std::floor(pieces[p].scrap));
        if (scrap > 0) {
            hold.capacity += scrap;
            hold.add(content::kGoodMetals, scrap);
            m_stats.scrapCreated += scrap;
        }
        if (hold.used() == 0) {
            continue; // nothing worth going for
        }
        salvageable += hold.used();
        const EntityId wreck = world.createEntity();
        // It drifts with the flight system (coasting, no drive): positions stay in step with the ships that
        // come to salvage it.
        world.components<Kinematics>().add(wreck,
                                           {kinematics.position + pieces[p].offset, pieces[p].velocity, {}});
        ShipControl drift;
        drift.mode = FlightMode::Coast;
        world.components<ShipControl>().add(wreck, drift);
        world.components<ShipDrive>().add(wreck, {});
        world.components<CargoHold>().add(wreck, std::move(hold));
        world.components<Wreck>().add(
            wreck, {name, shipClass, faction, now, now + content::kWreckLifetime, known, pieces[p].hulk});
        ++m_stats.wrecksFormed;
    }
    m_debris.push_back({kinematics.position, kinematics.velocity, now,
                        designSize(shipClass) / designSize(content::kShipClassHauler)});
    if (salvageable > 0 && (known >> content::kFactionPlayer & 1u) != 0 &&
        (isPlayerShip(world, event.ship) || isPlayerShip(world, event.attacker))) {
        addJournal(now, msg("Quedan restos de {} a la deriva: {} t recuperables.", literal(name),
                            number(salvageable)));
    }
}

void Sandbox::updateWrecks(const TickContext& context) {
    World& world = context.world;
    const SimTime now = context.now;
    const f64 dt = context.dt.toSeconds();
    ComponentStore<Wreck>& wrecks = world.components<Wreck>();
    ComponentStore<Kinematics>& kinematics = world.components<Kinematics>();

    // The company's ships find the wrecks they pass close to (wrecks drift with the flight system).
    std::vector<Vec3d> lookouts;
    const ComponentStore<ShipIdentity>& identities = world.components<ShipIdentity>();
    for (usize i = 0; i < identities.size(); ++i) {
        if (identities.values()[i].faction == content::kFactionPlayer) {
            if (const Kinematics* state = kinematics.tryGet(identities.entities()[i])) {
                lookouts.push_back(state->position);
            }
        }
    }
    for (usize i = 0; i < wrecks.size(); ++i) {
        const Kinematics& state = kinematics.get(wrecks.entities()[i]);
        Wreck& wreck = wrecks.values()[i];
        if (!wreck.knownTo(content::kFactionPlayer) &&
            std::any_of(lookouts.begin(), lookouts.end(), [&](const Vec3d& lookout) {
                return lengthSquared(lookout - state.position) <
                       content::kWreckSightRange * content::kWreckSightRange;
            })) {
            wreck.knownBy |= 1u << content::kFactionPlayer;
        }
    }

    // Debris: ships crossing a cloud fast are struck (the path of the last `dt`, in a straight line).
    std::erase_if(m_debris, [&](const DebrisCloud& cloud) { return debrisExpired(cloud, now); });
    if (m_debris.empty() || dt <= 0.0) {
        return;
    }
    const ComponentStore<ShipControl>& controls = world.components<ShipControl>();
    u64 salt = 0;
    for (usize c = 0; c < m_debris.size(); ++c) {
        const DebrisCloud& cloud = m_debris[c];
        const Vec3d center = cloud.centerAt(now);
        const f64 radius = debrisRadius(cloud, now);
        for (usize i = 0; i < controls.size(); ++i) {
            const EntityId ship = controls.entities()[i];
            const Kinematics* state = kinematics.tryGet(ship);
            if (state == nullptr || controls.values()[i].phase == DrivePhase::Hyperspace ||
                wrecks.contains(ship)) {
                continue;
            }
            // The path through the cloud, in the cloud's frame (it drifts too).
            const Vec3d from = state->position - (state->velocity - cloud.velocity) * dt;
            // Cheap rejection: the whole path is far from the cloud.
            const f64 reach = radius + length(state->velocity) * dt;
            if (lengthSquared(state->position - center) > reach * reach) {
                continue;
            }
            Rng rng = Rng::forStream(context.worldSeed, hashCombine(kDebrisStream, entityKey(ship)),
                                     hashCombine(context.runIndex, c));
            for (const f64 damage : debrisImpacts(cloud, now, from, state->position, state->velocity, rng)) {
                m_combat.applyHit(context, ship, {}, damage, salt++);
                ++m_stats.debrisHits;
            }
        }
    }
}

void Sandbox::removeWrecks(World& world, SimTime now) {
    std::vector<EntityId> gone;
    const ComponentStore<Wreck>& wrecks = world.components<Wreck>();
    for (usize i = 0; i < wrecks.size(); ++i) {
        const EntityId wreck = wrecks.entities()[i];
        const CargoHold& hold = world.components<CargoHold>().get(wreck);
        if (hold.used() == 0 || now >= wrecks.values()[i].expires) {
            gone.push_back(wreck);
        }
    }
    for (const EntityId wreck : gone) {
        for (const CargoItem& item : world.components<CargoHold>().get(wreck).items) {
            m_stats.cargoLost[item.good] += item.tonnes; // out of reach for good
        }
        world.destroyEntity(wreck);
    }
}

u32 Sandbox::salvage(World& world, EntityId ship, EntityId wreck, SimTime now) {
    CargoHold* from = world.components<CargoHold>().tryGet(wreck);
    CargoHold* to = world.components<CargoHold>().tryGet(ship);
    if (from == nullptr || to == nullptr) {
        return 0;
    }
    u32 taken = 0;
    std::optional<Message> list;
    const std::vector<CargoItem> items = from->items; // taking edits the wreck's hold
    for (const CargoItem& item : items) {
        const u32 tonnes = std::min(item.tonnes, to->space());
        if (tonnes == 0) {
            continue;
        }
        to->add(item.good, tonnes);
        from->remove(item.good, tonnes);
        taken += tonnes;
        Message part = msg("{} t de {}", number(tonnes), term(m_economy.goods()[item.good].name));
        list = list.has_value() ? msg("{}, {}", std::move(*list), std::move(part)) : std::move(part);
    }
    m_stats.tonnesSalvaged += taken;
    if (taken > 0 && isPlayerShip(world, ship)) {
        m_company.totals.tonnesSalvaged += taken;
        const std::string& name = world.components<Wreck>().get(wreck).name;
        addJournal(now,
                   ship == m_player ? msg("Recuperas de los restos de {}: {}.", literal(name), *list)
                                    : msg("{} recupera de los restos de {}: {}.", named(world, ship),
                                          literal(name), *list),
                   ship == m_player ? JournalKind::Player : JournalKind::Fleet);
    }
    return taken;
}

void Sandbox::onSalvageCommand(const SalvageCommand& command, const TickContext& context) {
    World& world = context.world;
    const auto reject = [&](const char* reason, const char* message) {
        ++m_stats.commandsRejected;
        GX_LOG_WARN("Sandbox", "salvage command rejected: {}", reason);
        addJournal(context.now, msg(message));
    };
    CargoHold* hold = command.ship == m_player && world.isAlive(m_player)
                          ? world.components<CargoHold>().tryGet(m_player)
                          : nullptr;
    if (hold == nullptr || hold->capacity == 0) {
        reject("no hold", GX_TEXT("Tu nave no tiene bodega para recuperar nada."));
        return;
    }
    const Wreck* wreck =
        world.isAlive(command.wreck) ? world.components<Wreck>().tryGet(command.wreck) : nullptr;
    if (wreck == nullptr || !wreck->knownTo(content::kFactionPlayer)) {
        reject("no wreck", GX_TEXT("No hay restos ahí."));
        return;
    }
    const Kinematics& mine = world.components<Kinematics>().get(m_player);
    const Kinematics& theirs = world.components<Kinematics>().get(command.wreck);
    if (length(theirs.position - mine.position) > content::kSalvageRange) {
        reject("too far", GX_TEXT("Demasiado lejos: acércate a menos de 5 km de los restos."));
        return;
    }
    if (length(theirs.velocity - mine.velocity) > content::kSalvageSpeed) {
        reject("too fast", GX_TEXT("Iguala la velocidad con los restos para recuperarlos."));
        return;
    }
    if (hold->space() == 0) {
        reject("hold full", GX_TEXT("La bodega está llena."));
        return;
    }
    salvage(world, m_player, command.wreck, context.now);
}

EntityId Sandbox::nearestKnownWreck(const World& world, const Vec3d& position, EntityId except) const {
    // Wrecks another company salvager is already on its way to are left to it.
    std::vector<EntityId> taken;
    const ComponentStore<FleetBrain>& brains = world.components<FleetBrain>();
    for (usize i = 0; i < brains.size(); ++i) {
        const EntityId ship = brains.entities()[i];
        const ShipControl& control = world.components<ShipControl>().get(ship);
        if (ship != except && brains.values()[i].order == FleetOrder::Salvage &&
            control.mode == FlightMode::Approach) {
            taken.push_back(control.target);
        }
    }
    const ComponentStore<Wreck>& wrecks = world.components<Wreck>();
    EntityId best;
    f64 bestDistance = 0.0;
    for (usize i = 0; i < wrecks.size(); ++i) {
        const EntityId wreck = wrecks.entities()[i];
        if (!wrecks.values()[i].knownTo(content::kFactionPlayer) ||
            world.components<CargoHold>().get(wreck).used() == 0 ||
            std::find(taken.begin(), taken.end(), wreck) != taken.end()) {
            continue;
        }
        const f64 distance = length(world.components<Kinematics>().get(wreck).position - position);
        if (!best.isValid() || distance < bestDistance) {
            best = wreck;
            bestDistance = distance;
        }
    }
    return best;
}

void Sandbox::fleetSalvage(World& world, EntityId ship, FleetBrain& brain, SimTime now) {
    CargoHold& hold = world.components<CargoHold>().get(ship);
    const ShipControl& control = world.components<ShipControl>().get(ship);
    const EntityId docked = dockedPort(world, ship);
    const auto goSell = [&](EntityId except) {
        // Where the most valuable part of the load sells best (the rest goes with it).
        GoodId good = 0;
        f64 value = -1.0;
        for (const CargoItem& item : hold.items) {
            const f64 itemValue = item.tonnes * m_economy.basePrice(item.good);
            if (itemValue > value) {
                value = itemValue;
                good = item.good;
            }
        }
        const EntityId market =
            bestMarket(world, ship, good, static_cast<f64>(hold.amount(good)), now, except);
        if (market.isValid()) {
            sendTo(world, ship, brain, market);
            ++brain.trips;
        } else {
            brain.task = FleetTask::Idle;
        }
    };

    if (brain.task == FleetTask::Travelling) {
        const bool toWreck = world.components<Wreck>().contains(control.target);
        if (!world.isAlive(control.target) || control.mode != FlightMode::Approach) {
            brain.task = FleetTask::Idle; // the wreck was emptied or drifted away
        } else if (!control.arrived) {
            return;
        } else if (docked.isValid() && control.target == docked) {
            sellFleetCargo(world, ship, docked, now);
            brain.task = FleetTask::Idle;
            if (hold.used() * 2 > hold.capacity) {
                goSell(docked); // that market is full: another one
                return;
            }
        } else if (toWreck) {
            salvage(world, ship, control.target, now);
            brain.task = FleetTask::Idle;
            if (brain.site == control.target) {
                brain.site = {}; // done with the one it was sent to: any other now
            }
        } else {
            brain.task = FleetTask::Idle;
        }
    }
    if (brain.task != FleetTask::Idle) {
        return;
    }
    if (hold.space() == 0) {
        goSell({});
        return;
    }
    const Vec3d position = world.components<Kinematics>().get(ship).position;
    EntityId target = brain.site;
    if (!world.isAlive(target) || !world.components<Wreck>().contains(target) ||
        world.components<CargoHold>().get(target).used() == 0) {
        brain.site = {};
        target = nearestKnownWreck(world, position, ship);
    }
    if (target.isValid()) {
        sendTo(world, ship, brain, target);
    } else if (hold.used() > 0) {
        goSell({});
    } else if (!docked.isValid()) {
        const EntityId station = nearestStation(world, position, now);
        if (station.isValid()) {
            sendTo(world, ship, brain, station); // wait at a station for news of wrecks
        }
    }
}

} // namespace gx
