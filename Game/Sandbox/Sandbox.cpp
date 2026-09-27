#include "Game/Sandbox/Sandbox.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Random.h"
#include "Engine/Serialization/Binary.h"
#include "Game/Sandbox/Content.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Bodies/CelestialBody.h"
#include "Space/Generation/StarSystemGenerator.h"
#include "Space/Ships/Modules.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace gx {
namespace {

constexpr u64 kSpawnStream = fnv1a64("sandbox.spawn");
constexpr u64 kRespawnStream = fnv1a64("sandbox.respawn");
constexpr u64 kRouteStream = fnv1a64("sandbox.hauler.route");
constexpr u64 kDwellStream = fnv1a64("sandbox.hauler.dwell");
constexpr u64 kAmbushStream = fnv1a64("sandbox.pirate.ambush");
constexpr SimDuration kAmbushTime = SimDuration::minutes(4); // raiders move to another ambush point
constexpr SimDuration kHitJournalGap = SimDuration::seconds(10);
constexpr f64 kAmbushWellFactor = 1.1; // raiders wait just outside a planet's gravity well
constexpr usize kAmbushChoices = 3;    // raiders move between the planets nearest to them
constexpr SimDuration kHuntTimeout = SimDuration::minutes(3); // then the prey is not worth it
constexpr f64 kExitRadiusFactor = 1.3; // raiders leave beyond the outermost planet's apoapsis

u64 entityKey(EntityId entity) {
    return (static_cast<u64>(entity.generation) << 32) | entity.index;
}

bool isFinite(const Vec3d& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Vec3d clampToUnit(const Vec3d& v) {
    const f64 len = length(v);
    return len > 1.0 ? v / len : v;
}

const char* moduleName(ModuleType type) {
    const auto index = static_cast<usize>(type);
    return index < content::kModuleNames.size() ? content::kModuleNames[index] : "?";
}

// New orders keep the drive's state: a ship in hyperspace finishes or aborts its jump through flyShip.
void resetOrders(ShipControl& control, FlightMode mode) {
    const DrivePhase phase = control.phase;
    const f64 charge = control.chargeRemaining;
    control = {};
    control.mode = mode;
    control.phase = phase;
    control.chargeRemaining = charge;
}

// Random point on a circle of `radius` around `centre`, in the ecliptic plane.
Vec3d pointAround(const Vec3d& centre, f64 radius, Rng& rng) {
    const f64 angle = rng.uniform(0.0, kTwoPi);
    return centre + Vec3d{std::cos(angle), std::sin(angle), 0.0} * radius;
}

} // namespace

const char* toString(PirateState state) {
    switch (state) {
    case PirateState::Lurking:
        return "Lurking";
    case PirateState::Hunting:
        return "Hunting";
    case PirateState::Leaving:
        return "Leaving";
    case PirateState::Count:
        break;
    }
    return "?";
}

Sandbox::Sandbox(const SandboxConfig& config) : m_config(config) {}

bool Sandbox::tactical() const {
    return m_simulation != nullptr && m_flightSystem != kInvalidSystemId &&
           m_simulation->scheduler().system(m_flightSystem).desc.period == m_config.tacticalFlightPeriod;
}

void Sandbox::install(Simulation& simulation) {
    m_simulation = &simulation;
    registerSpaceTypes(simulation);
    SensorSystem::registerTypes(simulation);
    CombatSystem::registerTypes(simulation);
    EconomySystem::registerTypes(simulation);
    World& world = simulation.world();
    world.registerComponent<HaulerBrain>("Game.HaulerBrain");
    world.registerComponent<PirateBrain>("Game.PirateBrain");

    EventBus& events = simulation.events();
    events.channel<ShipArrived>().subscribe(
        [this](const ShipArrived& event, const TickContext& context) { onShipArrived(event, context); });
    events.channel<HyperspaceTransition>().subscribe(
        [this](const HyperspaceTransition& event, const TickContext& context) {
            onHyperspaceTransition(event, context);
        });
    events.channel<ShipDamaged>().subscribe(
        [this](const ShipDamaged& event, const TickContext& context) { onShipDamaged(event, context); });
    events.channel<ShipDestroyed>().subscribe(
        [this](const ShipDestroyed& event, const TickContext& context) { onShipDestroyed(event, context); });

    CommandQueue& commands = simulation.commands();
    commands.registerCommand<PilotCommand>("Game.Pilot",
                                           [this](const PilotCommand& command, const TickContext& context) {
                                               onPilotCommand(command, context);
                                           });
    commands.registerCommand<SensorCommand>("Game.Sensors",
                                            [this](const SensorCommand& command, const TickContext& context) {
                                                onSensorCommand(command, context);
                                            });
    commands.registerCommand<EngageCommand>("Game.Engage",
                                            [this](const EngageCommand& command, const TickContext& context) {
                                                onEngageCommand(command, context);
                                            });
    commands.registerCommand<TradeCommand>("Game.Trade",
                                           [this](const TradeCommand& command, const TickContext& context) {
                                               onTradeCommand(command, context);
                                           });

    // Behaviour runs before flight in the same phase, so new orders fly in the same step. Sensors scan after
    // flight (they see this step's motion) and combat fires after both, on the flight cadence.
    simulation.addSystem({"Game.Haulers",
                          TickPhase::Simulation,
                          SimDuration::seconds(10),
                          {},
                          [this](const TickContext& context) { updateHaulers(context); }});
    simulation.addSystem({"Game.Pirates",
                          TickPhase::Simulation,
                          SimDuration::seconds(1),
                          {},
                          [this](const TickContext& context) { updatePirates(context); }});
    m_flight.setSensors(&m_sensors);
    m_flightSystem = m_flight.install(simulation, m_config.strategicFlightPeriod);
    m_sensors.install(simulation, m_config.sensorScanPeriod);
    m_combat.setWeapons(content::weaponTable());
    m_combat.install(simulation, m_sensors, m_config.strategicFlightPeriod);
    m_economy.setGoods(content::goodTable());
    m_economy.install(simulation, m_config.economyPeriod);
    // Repairs, respawns and departures of raiders: structural changes, so after the event subscribers.
    simulation.addSystem({"Game.Upkeep",
                          TickPhase::EventResolution,
                          SimDuration::seconds(1),
                          {},
                          [this](const TickContext& context) { updateUpkeep(context); }});

    simulation.addStateBlock(
        "Game.Sandbox", [this](BinaryWriter& writer) { writeState(writer); },
        [this](BinaryReader& reader) { readState(reader); });
}

void Sandbox::populate(Simulation& simulation) {
    World& world = simulation.world();
    const SimTime now = simulation.now();
    const StarSystemDesc system = generateStarSystem(m_config.seed);
    spawnStarSystem(world, system);
    m_systemName = system.name;
    rebuildPorts(world);
    GX_CHECK(!m_ports.empty() && !m_planets.empty(), "the generated system has no ports");
    setupMarkets(world);

    Rng rng = Rng::forStream(m_config.seed, kSpawnStream);
    m_player = spawnShip(world, now, rng, content::kPlayerShipName, content::kFactionPlayer,
                         content::kShipClassCourier, m_home);
    for (u32 i = 0; i < m_config.haulers; ++i) {
        spawnHauler(world, now, rng, i);
    }
    for (u32 i = 0; i < m_config.pirates; ++i) {
        spawnPirate(world, now, rng, i);
    }
    m_nextHaulerSpawn = now;
    m_nextPirateSpawn = now;
    m_playerCredits = content::kPlayerStartCredits;
    m_stats.cargoLost.assign(content::kGoodCount, 0);
    // At the start everybody has the system's market bulletin; from then on knowledge travels with ships.
    for (const EntityId port : m_ports) {
        const Market& market = world.components<Market>().get(port);
        m_traderPrices.observe(port, market, m_economy.goods(), now);
        m_playerPrices.observe(port, market, m_economy.goods(), now);
    }
    addJournal(now, std::format("Comienza la partida en el sistema {}.", m_systemName));
}

EntityId Sandbox::spawnShip(World& world, SimTime now, Rng& rng, std::string name, u32 faction, u32 shipClass,
                            EntityId port) {
    // Docked at the port: keeping station at a random point of its standoff circle.
    const OrbitState portState = bodyStateAt(world, port, now);
    const f64 standoff = standoffDistance(world, port);
    ShipControl control;
    control.mode = FlightMode::Approach;
    control.target = port;
    control.standoff = standoff;
    control.arrived = true;

    const content::ShipClassDef& shipDef = content::kShipClasses[shipClass];
    const content::SensorDef& sensorDef = content::kShipSensors[shipClass];
    const EntityId ship = world.createEntity();
    world.components<Kinematics>().add(
        ship, {pointAround(portState.position, standoff, rng), portState.velocity, {}});
    world.components<ShipDrive>().add(ship, {shipDef.maxAcceleration, shipDef.cruiseSpeed,
                                             shipDef.hyperspaceSpeed, shipDef.hyperspaceChargeTime});
    world.components<ShipControl>().add(ship, control);
    world.components<ShipIdentity>().add(ship, {std::move(name), faction, shipClass});
    world.components<SensorSuite>().add(
        ship, {sensorDef.passiveSensitivity, sensorDef.activeStrength, false, true});
    world.components<SignatureProfile>().add(
        ship, {sensorDef.baseEmission, sensorDef.driveEmission, sensorDef.crossSection});

    ShipModules modules;
    bool armed = false;
    for (const content::ModuleDef& module : content::shipModules(shipClass)) {
        modules.modules.push_back({module.type, module.weapon, module.health, module.health, 0.0});
        armed = armed || module.type == ModuleType::Weapon;
    }
    world.components<ShipModules>().add(ship, std::move(modules));
    world.components<ShipDesignStats>().add(ship, {shipDef.maxAcceleration, shipDef.cruiseSpeed,
                                                   shipDef.hyperspaceSpeed, shipDef.hyperspaceChargeTime,
                                                   sensorDef.passiveSensitivity, sensorDef.activeStrength,
                                                   shipDef.hullRadius});
    if (armed) {
        world.components<CombatControl>().add(ship, {});
    }
    world.components<CargoHold>().add(ship, {content::kCargoCapacity[shipClass], {}});
    if (faction == content::kFactionPlayer) {
        world.components<Wallet>().add(ship, {content::kPlayerStartCredits});
    } else if (faction == content::kFactionIndependent) {
        world.components<Wallet>().add(ship, {content::kHaulerStartCredits});
    }
    return ship;
}

void Sandbox::setupMarkets(World& world) {
    ComponentStore<Market>& markets = world.components<Market>();
    const ComponentStore<CelestialBody>& bodies = world.components<CelestialBody>();
    for (const EntityId port : m_ports) {
        content::PortRole role = content::PortRole::Planet;
        for (usize s = 0; s < m_stations.size(); ++s) {
            if (m_stations[s] == port) {
                role = m_stations.size() == 1
                           ? content::PortRole::Industry
                           : (s % 2 == 0 ? content::PortRole::Refinery : content::PortRole::Factory);
            }
        }
        markets.add(port, content::portMarket(bodies.get(port).kind, role));
    }
    // Enough of everything in aggregate (kSupplyMargin over total use), walking the chain downstream-first:
    // machinery sets the factory's pace, the factory the refinery's, and both the raw goods'. Existing
    // producers are scaled up, so geography still decides where goods come from; a good nobody can extract is
    // made up for by the first station. Local shortages are then a matter of transport, not of the
    // generator's luck.
    const auto total = [&](GoodId good, auto rateOf) {
        f64 sum = 0.0;
        for (const EntityId port : m_ports) {
            sum += rateOf(markets.get(port), good);
        }
        return sum;
    };
    // More extraction needs more machinery upkeep, so repeat until nothing changes (two passes in practice).
    for (bool changed = true; changed;) {
        changed = false;
        for (const content::Good good : {content::kGoodMachinery, content::kGoodMetals, content::kGoodWater,
                                         content::kGoodFood, content::kGoodOre, content::kGoodFuel}) {
            const f64 needed = total(good, consumptionRate) * content::kSupplyMargin;
            const f64 supplied = total(good, productionRate);
            if (supplied >= needed * (1.0 - 1e-9) || needed <= 0.0) {
                continue;
            }
            if (supplied > 0.0) {
                for (const EntityId port : m_ports) {
                    for (Recipe& recipe : markets.get(port).recipes) {
                        recipe.rate *= recipe.output == good ? needed / supplied : 1.0;
                    }
                }
                changed = true;
            } else if (!m_stations.empty()) {
                markets.get(m_stations.front())
                    .recipes.push_back({good, needed, {{content::kGoodMachinery, 0.02, false}}});
                changed = true;
            } // else: nowhere to make it (no stations)
        }
    }
    // Listed goods, target stocks and storage from the rates; markets start at their target (base prices).
    m_initialStock.assign(content::kGoodCount, 0.0);
    for (const EntityId port : m_ports) {
        Market& market = markets.get(port);
        for (const Recipe& recipe : market.recipes) {
            market.ensure(recipe.output);
            for (const RecipeInput& input : recipe.inputs) {
                market.ensure(input.good);
            }
        }
        for (const Demand& demand : market.demands) {
            market.ensure(demand.good);
        }
        for (MarketGood& good : market.goods) {
            const f64 rate = std::max(productionRate(market, good.good), consumptionRate(market, good.good));
            good.target = std::max(content::kMarketMinTarget, rate * content::kMarketStockHours);
            good.capacity = good.target * content::kMarketCapacityFactor;
            good.stock = good.target;
            m_initialStock[good.good] += good.stock;
        }
    }
}

EntityId Sandbox::dockedPort(const World& world, EntityId ship) const {
    const ShipControl* control = world.components<ShipControl>().tryGet(ship);
    if (control == nullptr || control->mode != FlightMode::Approach || !control->arrived) {
        return {};
    }
    return world.components<Market>().contains(control->target) ? control->target : EntityId{};
}

f64 Sandbox::danger(EntityId port, SimTime now) const {
    for (const PortDanger& entry : m_danger) {
        if (entry.port == port) {
            return entry.level * std::exp2(-(now - entry.updated).toSeconds() / content::kDangerHalfLife);
        }
    }
    return 0.0;
}

void Sandbox::addDanger(EntityId port, SimTime now) {
    const f64 current = danger(port, now);
    for (PortDanger& entry : m_danger) {
        if (entry.port == port) {
            entry.level = current + 1.0;
            entry.updated = now;
            return;
        }
    }
    m_danger.push_back({port, 1.0, now});
}

void Sandbox::planHaulerTrip(World& world, EntityId ship, HaulerBrain& brain, ShipControl& control,
                             SimTime now, Rng& rng) {
    CargoHold& hold = world.components<CargoHold>().get(ship);
    Wallet& wallet = world.components<Wallet>().get(ship);
    const EntityId here = dockedPort(world, ship);
    Market* market = here.isValid() ? world.components<Market>().tryGet(here) : nullptr;
    const std::vector<GoodDef>& goods = m_economy.goods();
    if (market != nullptr) {
        m_traderPrices.observe(here, *market, goods, now);
    }

    // Every option is judged per second of trip, discounted by stale knowledge and by recent losses there.
    const Vec3d position = world.components<Kinematics>().get(ship).position;
    const f64 hyperspaceSpeed = content::kShipClasses[content::kShipClassHauler].hyperspaceSpeed;
    struct Option {
        EntityId port;
        const PortPrices* known = nullptr;
        Vec3d position;
        f64 seconds = 0.0;   // trip estimate from here
        f64 freshness = 0.0; // 1 for prices seen now, 1/2 after kKnowledgeHalfLife
        f64 danger = 0.0;
        f64 weight = 0.0; // freshness / (seconds * (1 + danger))
    };
    std::vector<Option> options;
    for (const PortPrices& known : m_traderPrices.ports) {
        if (known.port == here || !world.isAlive(known.port)) {
            continue;
        }
        Option option{known.port, &known, bodyStateAt(world, known.port, now).position};
        option.seconds = content::kTripOverhead + length(option.position - position) / hyperspaceSpeed;
        option.freshness = std::exp2(-(now - known.observed).toSeconds() / content::kKnowledgeHalfLife);
        option.danger = danger(known.port, now);
        option.weight = option.freshness / (option.seconds * (1.0 + option.danger));
        options.push_back(option);
    }
    const auto inflight = [&](EntityId port, GoodId good) {
        for (const Delivery& delivery : m_inflight) {
            if (delivery.port == port && delivery.good == good) {
                return delivery.tonnes;
            }
        }
        return 0.0;
    };
    // What `tonnes` would fetch at a known market: the price moves as they are sold, after the deliveries the
    // traders already have on their way there, and the market takes no more than it can store.
    const auto revenue = [&](const PortPrices& known, GoodId good, f64 tonnes) {
        const PricePoint* bid = known.find(good);
        if (bid == nullptr || bid->target <= 0.0) {
            return 0.0;
        }
        const f64 stock = bid->stockRatio * bid->target + inflight(known.port, good);
        const f64 sellable = std::clamp(bid->target * content::kMarketCapacityFactor - stock, 0.0, tonnes);
        return sellable * unitPrice(m_economy.basePrice(good), stock + sellable / 2.0, bid->target) *
               (1.0 - kMarketSpread);
    };

    EntityId destination;
    GoodId load = 0;
    u32 loadTonnes = 0;
    f64 bestScore = 0.0;
    if (hold.used() > 0) {
        // Loaded (a sale fell short, or it fled here): where the cargo sells best.
        for (const Option& option : options) {
            f64 value = 0.0;
            for (const CargoItem& item : hold.items) {
                value += revenue(*option.known, item.good, item.tonnes);
            }
            if (value * option.weight > bestScore) {
                bestScore = value * option.weight;
                destination = option.port;
            }
        }
    } else if (market != nullptr) {
        // Empty at a market: the best load here for a port that pays more for it.
        for (MarketGood& good : market->goods) {
            const f64 base = m_economy.basePrice(good.good);
            const i64 unit = buyPrice(good, base);
            const auto cap = static_cast<u32>(
                std::min<f64>(hold.space(), std::floor(good.stock * content::kHaulerMaxMarketShare)));
            u32 tonnes = std::min<u32>(cap, static_cast<u32>(std::max<i64>(0, wallet.credits / unit)));
            TradeResult quote = quoteBuy(good, base, tonnes);
            while (tonnes > 0 && quote.credits > wallet.credits) {
                tonnes = tonnes * 9 / 10;
                quote = quoteBuy(good, base, tonnes);
            }
            if (tonnes == 0) {
                continue;
            }
            for (const Option& option : options) {
                const f64 profit =
                    revenue(*option.known, good.good, tonnes) - static_cast<f64>(quote.credits);
                if (profit >= static_cast<f64>(content::kHaulerMinProfit) &&
                    profit * option.weight > bestScore) {
                    bestScore = profit * option.weight;
                    destination = option.port;
                    load = good.good;
                    loadTonnes = tonnes;
                }
            }
        }
    }
    if (!destination.isValid() && hold.used() == 0) {
        // Nothing worth loading here: fly empty to where a known bargain can be bought and sold elsewhere.
        for (const Option& source : options) {
            for (const PricePoint& offer : source.known->prices) {
                if (offer.target <= 0.0) {
                    continue;
                }
                const f64 base = m_economy.basePrice(offer.good);
                const f64 stock = offer.stockRatio * offer.target;
                f64 tonnes = std::min<f64>(hold.capacity, std::floor(stock * content::kHaulerMaxMarketShare));
                const f64 unit = unitPrice(base, stock - tonnes / 2.0, offer.target) * (1.0 + kMarketSpread);
                tonnes = std::min(tonnes, std::floor(static_cast<f64>(wallet.credits) / unit));
                if (tonnes < 1.0) {
                    continue;
                }
                for (const Option& buyer : options) {
                    if (buyer.port == source.port) {
                        continue;
                    }
                    const f64 profit = revenue(*buyer.known, offer.good, tonnes) - tonnes * unit;
                    if (profit < static_cast<f64>(content::kHaulerMinProfit)) {
                        continue;
                    }
                    const f64 seconds = source.seconds + content::kTripOverhead +
                                        length(buyer.position - source.position) / hyperspaceSpeed;
                    const f64 score = profit * source.freshness * buyer.freshness /
                                      (seconds * (1.0 + source.danger + buyer.danger));
                    if (score > bestScore) {
                        bestScore = score;
                        destination = source.port;
                    }
                }
            }
        }
        m_stats.repositionTrips += destination.isValid() ? 1 : 0;
    }
    if (!destination.isValid()) {
        // Nothing worth carrying: refresh the oldest prices, or just move on.
        const PortPrices* oldest = nullptr;
        for (const Option& option : options) {
            if (oldest == nullptr || option.known->observed < oldest->observed) {
                oldest = option.known;
            }
        }
        if (oldest != nullptr && (now - oldest->observed).toSeconds() > content::kExploreAfter) {
            destination = oldest->port;
        } else {
            std::vector<EntityId> others;
            for (const EntityId port : m_ports) {
                if (port != here) {
                    others.push_back(port);
                }
            }
            destination = others[rng.uniformU32(static_cast<u32>(others.size()))];
        }
        ++m_stats.explorationTrips;
    }
    if (loadTonnes > 0) {
        const TradeResult bought =
            buyGoods(*market->find(load), m_economy.basePrice(load), loadTonnes, hold, wallet);
        m_stats.haulerTrades += bought.tonnes > 0 ? 1 : 0;
        m_traderPrices.observe(here, *market, goods, now);
    }
    // The network knows where this cargo is going: later plans in this run count on it.
    for (const CargoItem& item : hold.items) {
        addInflight(destination, item.good, item.tonnes);
    }
    control.mode = FlightMode::Approach;
    control.target = destination;
    control.standoff = standoffDistance(world, destination);
    control.arrived = false;
    ++brain.trips;
    ++m_stats.haulerDepartures;
}

void Sandbox::addInflight(EntityId port, GoodId good, f64 tonnes) {
    for (Delivery& delivery : m_inflight) {
        if (delivery.port == port && delivery.good == good) {
            delivery.tonnes += tonnes;
            return;
        }
    }
    m_inflight.push_back({port, good, tonnes});
}

void Sandbox::sellCargo(World& world, EntityId ship, EntityId port, SimTime now) {
    Market* market = world.components<Market>().tryGet(port);
    CargoHold* hold = world.components<CargoHold>().tryGet(ship);
    Wallet* wallet = world.components<Wallet>().tryGet(ship);
    if (market == nullptr || hold == nullptr || wallet == nullptr) {
        return;
    }
    const std::vector<CargoItem> items = hold->items; // selling edits the hold
    for (const CargoItem& item : items) {
        if (MarketGood* good = market->find(item.good)) {
            const TradeResult sold =
                sellGoods(*good, m_economy.basePrice(item.good), item.tonnes, *hold, *wallet);
            m_stats.tonnesDelivered += sold.tonnes;
        }
    }
    m_traderPrices.observe(port, *market, m_economy.goods(), now);
}

EntityId Sandbox::spawnHauler(World& world, SimTime now, Rng& rng, u32 serial) {
    const EntityId port = m_ports[rng.uniformU32(static_cast<u32>(m_ports.size()))];
    std::string name = std::format("{}-{}", content::kHaulerNames[serial % content::kHaulerNames.size()],
                                   10 + rng.uniformU32(90));
    const EntityId ship = spawnShip(world, now, rng, std::move(name), content::kFactionIndependent,
                                    content::kShipClassHauler, port);
    const auto maxDwellMs = static_cast<u32>(m_config.maxDwell.count() / 1000);
    world.components<HaulerBrain>().add(
        ship, {now + SimDuration::milliseconds(rng.uniformU32(maxDwellMs)), port, 0});
    return ship;
}

EntityId Sandbox::spawnPirate(World& world, SimTime now, Rng& rng, u32 serial) {
    const EntityId planet = m_planets[rng.uniformU32(static_cast<u32>(m_planets.size()))];
    std::string name = std::format("{}-{}", content::kRaiderNames[serial % content::kRaiderNames.size()],
                                   10 + rng.uniformU32(90));
    const EntityId ship = spawnShip(world, now, rng, std::move(name), content::kFactionPirates,
                                    content::kShipClassRaider, planet);
    // Not docked: waiting in ambush just outside the planet's well, silent (no transponder, no radar).
    const Vec3d point = ambushPoint(world, planet, now, rng);
    world.components<Kinematics>().get(ship) = {point, {}, {}};
    ShipControl& control = world.components<ShipControl>().get(ship);
    control = {};
    control.mode = FlightMode::MoveTo;
    control.point = point;
    control.arrived = true;
    world.components<SensorSuite>().get(ship).transponderOn = false;
    world.components<PirateBrain>().add(ship, {PirateState::Lurking, now + kAmbushTime, 0, 0});
    return ship;
}

Vec3d Sandbox::ambushPoint(const World& world, EntityId planet, SimTime now, Rng& rng) const {
    const CelestialBody& body = world.components<CelestialBody>().get(planet);
    return pointAround(bodyStateAt(world, planet, now).position, gravityWellRadius(body) * kAmbushWellFactor,
                       rng);
}

EntityId Sandbox::nearestStation(const World& world, const Vec3d& position, SimTime now) const {
    EntityId best;
    f64 bestDistance = 0.0;
    for (const EntityId station : m_stations) {
        const f64 distance = length(bodyStateAt(world, station, now).position - position);
        if (!best.isValid() || distance < bestDistance) {
            best = station;
            bestDistance = distance;
        }
    }
    return best;
}

void Sandbox::updateHaulers(const TickContext& context) {
    if (m_ports.size() < 2) {
        return;
    }
    World& world = context.world;
    ComponentStore<HaulerBrain>& brains = world.components<HaulerBrain>();
    ComponentStore<ShipControl>& controls = world.components<ShipControl>();
    // Deliveries already on their way, as the traders' network knows them.
    m_inflight.clear();
    const ComponentStore<CargoHold>& holds = world.components<CargoHold>();
    for (usize i = 0; i < brains.size(); ++i) {
        const ShipControl& control = controls.get(brains.entities()[i]);
        const CargoHold* hold = holds.tryGet(brains.entities()[i]);
        if (hold != nullptr && control.mode == FlightMode::Approach && !control.arrived) {
            for (const CargoItem& item : hold->items) {
                addInflight(control.target, item.good, item.tonnes);
            }
        }
    }
    for (usize i = 0; i < brains.size(); ++i) {
        const EntityId ship = brains.entities()[i];
        HaulerBrain& brain = brains.values()[i];
        ShipControl& control = controls.get(ship);
        const bool docked = control.arrived || control.mode == FlightMode::Stop;
        if (!docked || context.now < brain.departAt) {
            continue;
        }
        // Randomness keyed by (ship, trip): order-independent.
        Rng rng = Rng::forStream(context.worldSeed, hashCombine(kRouteStream, entityKey(ship)), brain.trips);
        planHaulerTrip(world, ship, brain, control, context.now, rng);
    }
}

void Sandbox::updatePirates(const TickContext& context) {
    World& world = context.world;
    ComponentStore<PirateBrain>& brains = world.components<PirateBrain>();
    if (brains.size() == 0) {
        return;
    }
    const SimTime now = context.now;
    // Charts are public: raiders know where the stations (and their guns) are.
    std::vector<Vec3d> stations;
    stations.reserve(m_stations.size());
    for (const EntityId station : m_stations) {
        stations.push_back(bodyStateAt(world, station, now).position);
    }
    const auto nearStation = [&](const Vec3d& position) {
        return std::any_of(stations.begin(), stations.end(), [&](const Vec3d& station) {
            return lengthSquared(position - station) <
                   content::kStationSafeRadius * content::kStationSafeRadius;
        });
    };
    const auto estimate = [&](const SensorContact& contact) {
        return contact.position + contact.velocity * (now - contact.lastSeen).toSeconds();
    };
    const FactionPicture& picture = m_sensors.picture(content::kFactionPirates);

    for (usize i = 0; i < brains.size(); ++i) {
        const EntityId ship = brains.entities()[i];
        PirateBrain& brain = brains.values()[i];
        ShipControl& control = world.components<ShipControl>().get(ship);
        CombatControl& orders = world.components<CombatControl>().get(ship);
        const ShipModules& modules = world.components<ShipModules>().get(ship);
        SensorSuite& suite = world.components<SensorSuite>().get(ship);
        const Vec3d position = world.components<Kinematics>().get(ship).position;
        if (!hasPower(modules)) {
            orders.targetTrack = 0; // disabled: drifting until damage control restores power
            continue;
        }
        if (control.phase == DrivePhase::Hyperspace) {
            continue; // nothing to decide until it drops out
        }

        const ShipModule* structure = structureOf(modules);
        const bool beaten = (structure != nullptr && structure->fraction() < content::kPirateFleeStructure) ||
                            moduleEfficiency(modules, ModuleType::Weapon) <= 0.0;
        if (brain.state != PirateState::Leaving && beaten) {
            // Out of the fight: leave the system through its outskirts (and out of the simulation).
            brain.state = PirateState::Leaving;
            orders.targetTrack = 0;
            suite.activeOn = false;
            const f64 distance = length(position);
            const Vec3d outward = distance > 1.0 ? position / distance : Vec3d{1.0, 0.0, 0.0};
            resetOrders(control, FlightMode::MoveTo);
            control.point = outward * m_exitRadius;
            continue;
        }

        switch (brain.state) {
        case PirateState::Leaving:
        case PirateState::Count:
            break;
        case PirateState::Hunting: {
            const SensorContact* prey = m_sensors.findContact(content::kFactionPirates, control.track);
            bool keep = prey != nullptr && control.mode == FlightMode::Pursue;
            if (keep) {
                const Vec3d where = estimate(*prey);
                keep = length(where - position) < content::kPirateGiveUpRange && !nearStation(where) &&
                       length(prey->velocity) < content::kHyperspaceSpeedThreshold;
            }
            if (keep && now >= brain.nextMove) {
                keep = false;
                brain.ignoreTrack = control.track; // too long for nothing: not this one again
            }
            if (!keep) {
                brain.state = PirateState::Lurking;
                brain.nextMove = now; // find another ambush point
                orders.targetTrack = 0;
                suite.activeOn = false;
                resetOrders(control, FlightMode::Stop);
            }
            break;
        }
        case PirateState::Lurking: {
            // Prey: the nearest track they can tell is not one of their own, outside hyperspace and away from
            // the stations. Ghosts look like anything else to them (and simply fade).
            const SensorContact* best = nullptr;
            f64 bestDistance = content::kPirateHuntRange;
            for (const SensorContact& contact : picture.contacts) {
                if (contact.level == ContactLevel::Unknown ||
                    contact.shipClass == content::kShipClassRaider || contact.trackId == brain.ignoreTrack ||
                    (contact.level == ContactLevel::Identified &&
                     contact.faction == content::kFactionPirates) ||
                    length(contact.velocity) >= content::kHyperspaceSpeedThreshold) {
                    continue;
                }
                const Vec3d where = estimate(contact);
                const f64 distance = length(where - position);
                if (distance < bestDistance && !nearStation(where)) {
                    best = &contact;
                    bestDistance = distance;
                }
            }
            if (best != nullptr) {
                brain.state = PirateState::Hunting;
                brain.nextMove = now + kHuntTimeout;
                ++m_stats.hunts;
                resetOrders(control, FlightMode::Pursue);
                control.track = best->trackId;
                control.standoff = content::kPirateStandoff;
                orders.targetTrack = best->trackId;
                suite.activeOn = suite.activeStrength > 0.0; // radar for fire control: now they are loud
            } else if (now >= brain.nextMove) {
                // Next ambush: one of the planets nearest to it. Outer planets are tens of minutes away in
                // hyperspace; a raider crossing the whole system at random would spend its life in transit.
                std::vector<std::pair<f64, u32>> nearest;
                nearest.reserve(m_planets.size());
                for (u32 p = 0; p < m_planets.size(); ++p) {
                    nearest.emplace_back(length(bodyStateAt(world, m_planets[p], now).position - position),
                                         p);
                }
                const auto choices = static_cast<u32>(std::min<usize>(kAmbushChoices, nearest.size()));
                std::partial_sort(nearest.begin(), nearest.begin() + choices, nearest.end());
                Rng rng = Rng::forStream(context.worldSeed, hashCombine(kAmbushStream, entityKey(ship)),
                                         brain.decisions++);
                const EntityId planet = m_planets[nearest[rng.uniformU32(choices)].second];
                resetOrders(control, FlightMode::MoveTo);
                control.point = ambushPoint(world, planet, now, rng);
                brain.nextMove = now + kAmbushTime;
            }
            break;
        }
        }
    }
}

void Sandbox::updateUpkeep(const TickContext& context) {
    World& world = context.world;
    const SimTime now = context.now;
    const f64 dt = context.dt.toSeconds();

    // Repairs: full service while docked at a station; elsewhere, once the shooting stops, damage control
    // patches the reactor and the drive up to a limp-home level.
    ComponentStore<ShipModules>& moduleStore = world.components<ShipModules>();
    const ComponentStore<ShipControl>& controls = world.components<ShipControl>();
    const ComponentStore<CelestialBody>& bodies = world.components<CelestialBody>();
    for (usize i = 0; i < moduleStore.size() && dt > 0.0; ++i) {
        const EntityId ship = moduleStore.entities()[i];
        ShipModules& modules = moduleStore.values()[i];
        const ShipControl* control = controls.tryGet(ship);
        const CelestialBody* port =
            control != nullptr && control->mode == FlightMode::Approach && control->arrived
                ? bodies.tryGet(control->target)
                : nullptr;
        const bool atStation = port != nullptr && port->kind == BodyKind::Station;
        const bool quiet = (now - modules.lastDamaged).toSeconds() >= content::kDamageControlDelay;
        bool changed = false;
        for (ShipModule& module : modules.modules) {
            f64 limit = module.maxHealth;
            f64 rate = content::kDockRepairRate;
            if (!atStation) {
                if (!quiet || (module.type != ModuleType::Reactor && module.type != ModuleType::Drive)) {
                    continue;
                }
                limit = module.maxHealth * content::kDamageControlCap;
                rate = content::kDamageControlRate;
            }
            if (module.health < limit) {
                module.health = std::min(limit, module.health + rate * module.maxHealth * dt);
                changed = true;
            }
        }
        if (changed) {
            applyModuleEffects(world, ship);
        }
    }

    // Raiders that made it out of the system leave the simulation.
    std::vector<EntityId> departed;
    const ComponentStore<PirateBrain>& pirates = world.components<PirateBrain>();
    for (usize i = 0; i < pirates.size(); ++i) {
        const EntityId ship = pirates.entities()[i];
        if (pirates.values()[i].state == PirateState::Leaving && controls.get(ship).arrived) {
            departed.push_back(ship);
        }
    }
    for (const EntityId ship : departed) {
        world.destroyEntity(ship);
        ++m_stats.piratesLeft;
        m_nextPirateSpawn = std::max(m_nextPirateSpawn, now + m_config.pirateRespawnDelay);
    }

    // Newcomers replace the losses, one at a time.
    Rng rng = Rng::forStream(m_config.seed, kRespawnStream, m_stats.spawns);
    if (!m_player.isValid() && now >= m_playerRespawnAt) {
        m_player = spawnShip(world, now, rng, content::kPlayerShipName, content::kFactionPlayer,
                             content::kShipClassCourier, m_home);
        world.components<Wallet>().get(m_player).credits = m_playerCredits;
        ++m_stats.spawns;
        addJournal(now, std::format("Una nave nueva te espera en {}.", nameOf(world, m_home)));
    }
    if (world.components<HaulerBrain>().size() < m_config.haulers && now >= m_nextHaulerSpawn) {
        spawnHauler(world, now, rng, static_cast<u32>(m_stats.spawns));
        ++m_stats.spawns;
        m_nextHaulerSpawn = now + m_config.haulerRespawnDelay;
    }
    if (world.components<PirateBrain>().size() < m_config.pirates && now >= m_nextPirateSpawn) {
        spawnPirate(world, now, rng, static_cast<u32>(m_stats.spawns));
        ++m_stats.spawns;
        m_nextPirateSpawn = now + m_config.pirateRespawnDelay;
    }

    updateFlightRate(world, now);
}

void Sandbox::onShipArrived(const ShipArrived& event, const TickContext& context) {
    World& world = context.world;
    ++m_stats.arrivals;
    if (HaulerBrain* brain = world.components<HaulerBrain>().tryGet(event.ship)) {
        brain->lastPort = event.target;
        sellCargo(world, event.ship, event.target, context.now);
        Rng rng =
            Rng::forStream(context.worldSeed, hashCombine(kDwellStream, entityKey(event.ship)), brain->trips);
        const auto dwellSpanMs = static_cast<u32>((m_config.maxDwell - m_config.minDwell).count() / 1000);
        brain->departAt =
            context.now + m_config.minDwell + SimDuration::milliseconds(rng.uniformU32(dwellSpanMs + 1));
        addJournal(context.now,
                   std::format("{} atraca en {}.", nameOf(world, event.ship), nameOf(world, event.target)));
    } else if (event.ship == m_player) {
        addJournal(context.now, event.target.isValid()
                                    ? std::format("{} ha llegado a {}.", nameOf(world, event.ship),
                                                  nameOf(world, event.target))
                                    : std::format("{} ha llegado a su destino.", nameOf(world, event.ship)));
        if (const Market* market = world.components<Market>().tryGet(event.target)) {
            m_playerPrices.observe(event.target, *market, m_economy.goods(), context.now);
            // Stations publish the traders' price bulletin.
            const CelestialBody* body = world.components<CelestialBody>().tryGet(event.target);
            if (body != nullptr && body->kind == BodyKind::Station) {
                if (const u32 updated = m_playerPrices.mergeNewer(m_traderPrices); updated > 0) {
                    addJournal(
                        context.now,
                        std::format("Boletín de precios de la estación: {} puertos actualizados.", updated));
                }
            }
        }
    }
}

void Sandbox::onHyperspaceTransition(const HyperspaceTransition& event, const TickContext& context) {
    if (event.ship != m_player) {
        return;
    }
    const ShipControl* control = context.world.components<ShipControl>().tryGet(event.ship);
    if (event.entering) {
        addJournal(context.now, "Salto al hiperespacio.");
    } else if (control != nullptr && control->mode == FlightMode::Approach) {
        addJournal(context.now, std::format("Salida del hiperespacio cerca de {}.",
                                            nameOf(context.world, control->target)));
    } else {
        addJournal(context.now, "Salida del hiperespacio.");
    }
}

void Sandbox::onShipDamaged(const ShipDamaged& event, const TickContext& context) {
    World& world = context.world;
    if (event.ship == m_player) {
        if (context.now - m_lastPlayerHit > kHitJournalGap) {
            addJournal(context.now, "¡Impacto! La nave está bajo fuego.");
        }
        m_lastPlayerHit = context.now;
        m_combatAlertUntil = context.now + m_config.combatAlert;
        if (event.moduleDestroyed) {
            addJournal(context.now, std::format("Módulo fuera de servicio: {}.", moduleName(event.module)));
        }
        updateFlightRate(world, context.now);
        return;
    }
    if (world.components<HaulerBrain>().contains(event.ship)) {
        // A hauler under fire runs for the nearest station's guns.
        ShipControl& control = world.components<ShipControl>().get(event.ship);
        const CelestialBody* target = world.components<CelestialBody>().tryGet(control.target);
        const bool toStation =
            control.mode == FlightMode::Approach && target != nullptr && target->kind == BodyKind::Station;
        if (!toStation) {
            const EntityId station =
                nearestStation(world, world.components<Kinematics>().get(event.ship).position, context.now);
            if (station.isValid()) {
                control.mode = FlightMode::Approach;
                control.target = station;
                control.standoff = standoffDistance(world, station);
                control.arrived = false;
            }
        }
    }
}

void Sandbox::onShipDestroyed(const ShipDestroyed& event, const TickContext& context) {
    const World& world = context.world;
    const SimTime now = context.now;
    if (const CargoHold* hold = world.components<CargoHold>().tryGet(event.ship)) {
        m_stats.cargoLost.resize(std::max<usize>(m_stats.cargoLost.size(), content::kGoodCount), 0);
        for (const CargoItem& item : hold->items) {
            m_stats.cargoLost[item.good] += item.tonnes; // the supply chain loses it for good
        }
    }
    if (const HaulerBrain* brain = world.components<HaulerBrain>().tryGet(event.ship)) {
        // The traders remember where they were heading: that route is dangerous for a while.
        const ShipControl& control = world.components<ShipControl>().get(event.ship);
        const EntityId port =
            control.mode == FlightMode::Approach && world.components<Market>().contains(control.target)
                ? control.target
                : brain->lastPort;
        if (port.isValid()) {
            addDanger(port, now);
        }
    }
    if (event.ship == m_player) {
        if (const Wallet* wallet = world.components<Wallet>().tryGet(event.ship)) {
            m_playerCredits = wallet->credits; // insured: the money survives the ship
        }
        addJournal(now, event.reactorBreach ? "¡Brecha en el reactor! Tu nave ha sido destruida."
                                            : "Tu nave ha sido destruida.");
        ++m_stats.playerDeaths;
        m_player = {};
        m_playerRespawnAt = now + m_config.playerRespawnDelay;
        return;
    }
    if (event.attacker.isValid() && event.attacker == m_player) {
        addJournal(now, "Objetivo destruido.");
    }
    if (const ShipIdentity* identity = world.components<ShipIdentity>().tryGet(event.ship)) {
        if (identity->faction == content::kFactionIndependent) {
            ++m_stats.haulersLost;
            m_nextHaulerSpawn = std::max(m_nextHaulerSpawn, now + m_config.haulerRespawnDelay);
        } else if (identity->faction == content::kFactionPirates) {
            ++m_stats.piratesLost;
            m_nextPirateSpawn = std::max(m_nextPirateSpawn, now + m_config.pirateRespawnDelay);
        }
    }
}

void Sandbox::onPilotCommand(const PilotCommand& command, const TickContext& context) {
    World& world = context.world;
    const auto reject = [&](const char* reason) {
        ++m_stats.commandsRejected;
        GX_LOG_WARN("Sandbox", "pilot command rejected: {}", reason);
    };
    const ShipIdentity* identity =
        world.isAlive(command.ship) ? world.components<ShipIdentity>().tryGet(command.ship) : nullptr;
    if (identity == nullptr || identity->faction != content::kFactionPlayer ||
        !world.components<ShipControl>().contains(command.ship)) {
        reject("not a ship of the player");
        return;
    }
    switch (command.mode) {
    case FlightMode::Coast:
    case FlightMode::Stop:
        break;
    case FlightMode::MoveTo:
        if (!isFinite(command.point) || length(command.point) > 1e15) {
            reject("invalid destination point");
            return;
        }
        break;
    case FlightMode::Approach:
        if (command.target == command.ship || !world.isAlive(command.target) ||
            !(world.components<CelestialBody>().contains(command.target) ||
              world.components<Kinematics>().contains(command.target))) {
            reject("invalid target");
            return;
        }
        break;
    case FlightMode::Manual:
        if (!isFinite(command.thrust)) {
            reject("invalid thrust");
            return;
        }
        break;
    default:
        reject("invalid flight mode"); // Pursue goes through EngageCommand
        return;
    }

    ShipControl& control = world.components<ShipControl>().get(command.ship);
    control.mode = command.mode;
    control.arrived = false;
    control.target = command.mode == FlightMode::Approach ? command.target : EntityId{};
    control.point = command.mode == FlightMode::MoveTo ? command.point : Vec3d{};
    control.manualThrust = command.mode == FlightMode::Manual ? clampToUnit(command.thrust) : Vec3d{};
    control.standoff = command.mode == FlightMode::Approach ? standoffDistance(world, command.target) : 0.0;
    control.track = 0;

    if (command.mode == FlightMode::Approach) {
        addJournal(context.now, std::format("Rumbo a {}.", nameOf(world, command.target)));
    } else if (command.mode == FlightMode::MoveTo) {
        addJournal(context.now, "Rumbo a un punto del espacio.");
    } else if (command.mode == FlightMode::Stop) {
        addJournal(context.now, "Deteniendo la nave.");
    }
    updateFlightRate(world, context.now);
}

void Sandbox::onSensorCommand(const SensorCommand& command, const TickContext& context) {
    World& world = context.world;
    const ShipIdentity* identity =
        world.isAlive(command.ship) ? world.components<ShipIdentity>().tryGet(command.ship) : nullptr;
    SensorSuite* suite = world.components<SensorSuite>().tryGet(command.ship);
    if (identity == nullptr || identity->faction != content::kFactionPlayer || suite == nullptr) {
        ++m_stats.commandsRejected;
        GX_LOG_WARN("Sandbox", "sensor command rejected: not a ship of the player");
        return;
    }
    const bool radarAvailable = suite->activeStrength > 0.0;
    if (command.activeOn != suite->activeOn && (radarAvailable || !command.activeOn)) {
        suite->activeOn = command.activeOn;
        addJournal(context.now, suite->activeOn ? "Radar activado: ves más, pero todos te ven a ti."
                                                : "Radar desactivado.");
    }
    if (command.transponderOn != suite->transponderOn) {
        suite->transponderOn = command.transponderOn;
        addJournal(context.now, suite->transponderOn
                                    ? "Transpondedor encendido."
                                    : "Transpondedor apagado: tu identidad ya no se difunde.");
    }
}

void Sandbox::onEngageCommand(const EngageCommand& command, const TickContext& context) {
    World& world = context.world;
    const auto reject = [&](const char* reason) {
        ++m_stats.commandsRejected;
        GX_LOG_WARN("Sandbox", "engage command rejected: {}", reason);
    };
    const ShipIdentity* identity =
        world.isAlive(command.ship) ? world.components<ShipIdentity>().tryGet(command.ship) : nullptr;
    CombatControl* orders = world.components<CombatControl>().tryGet(command.ship);
    if (identity == nullptr || identity->faction != content::kFactionPlayer || orders == nullptr) {
        reject("not an armed ship of the player");
        return;
    }
    if (command.track == 0 ? (command.fire || command.pursue)
                           : m_sensors.findContact(content::kFactionPlayer, command.track) == nullptr) {
        reject("unknown sensor track");
        return;
    }

    const u32 previous = orders->targetTrack;
    orders->targetTrack = command.fire ? command.track : 0;
    if (command.pursue) {
        ShipControl& control = world.components<ShipControl>().get(command.ship);
        resetOrders(control, FlightMode::Pursue);
        control.track = command.track;
        control.standoff = content::kPlayerPursuitStandoff;
    }
    if (command.fire) {
        addJournal(context.now, std::format("Fuego sobre el contacto {}.", command.track));
    } else if (command.pursue) {
        addJournal(context.now, std::format("Interceptando el contacto {}.", command.track));
    } else if (previous != 0) {
        addJournal(context.now, "Alto el fuego.");
    }
    updateFlightRate(world, context.now);
}

void Sandbox::onTradeCommand(const TradeCommand& command, const TickContext& context) {
    World& world = context.world;
    const auto reject = [&](const char* reason, const char* message) {
        ++m_stats.commandsRejected;
        GX_LOG_WARN("Sandbox", "trade command rejected: {}", reason);
        if (message != nullptr) {
            addJournal(context.now, message);
        }
    };
    const ShipIdentity* identity =
        world.isAlive(command.ship) ? world.components<ShipIdentity>().tryGet(command.ship) : nullptr;
    CargoHold* hold = world.components<CargoHold>().tryGet(command.ship);
    Wallet* wallet = world.components<Wallet>().tryGet(command.ship);
    if (identity == nullptr || identity->faction != content::kFactionPlayer || hold == nullptr ||
        wallet == nullptr) {
        reject("not a trading ship of the player", nullptr);
        return;
    }
    const EntityId port = dockedPort(world, command.ship);
    if (!port.isValid()) {
        reject("not docked at a port", "Para comerciar hay que estar atracado en un puerto.");
        return;
    }
    Market& market = world.components<Market>().get(port);
    MarketGood* good = market.find(command.good);
    if (good == nullptr || command.tonnes == 0) {
        reject("good not traded here", "Ese bien no se comercia en este puerto.");
        return;
    }
    const std::string& name = m_economy.goods()[command.good].name;
    const f64 base = m_economy.basePrice(command.good);
    if (command.tonnes > 0) {
        const TradeResult bought = buyGoods(*good, base, static_cast<u32>(command.tonnes), *hold, *wallet);
        if (bought.tonnes == 0) {
            reject("cannot buy", hold->space() == 0  ? "La bodega está llena."
                                 : good->stock < 1.0 ? "No quedan existencias."
                                                     : "No tienes créditos suficientes.");
            return;
        }
        addJournal(context.now,
                   std::format("Compras {} t de {} por {} cr.", bought.tonnes, name, bought.credits));
    } else {
        const TradeResult sold = sellGoods(*good, base, static_cast<u32>(-command.tonnes), *hold, *wallet);
        if (sold.tonnes == 0) {
            reject("cannot sell", hold->amount(command.good) == 0 ? "No llevas ese bien."
                                                                  : "El puerto no admite más de ese bien.");
            return;
        }
        addJournal(context.now, std::format("Vendes {} t de {} por {} cr.", sold.tonnes, name, sold.credits));
    }
    ++m_stats.playerTrades;
    m_playerPrices.observe(port, market, m_economy.goods(), context.now);
}

void Sandbox::updateFlightRate(const World& world, SimTime now) {
    // Fine flight and combat steps only while the player flies by hand or fights: responsive controls and
    // precise shooting without paying for 20 Hz integration during long autopilot trips. Requests during a
    // step apply at its end, in order.
    bool fine = now < m_combatAlertUntil;
    if (world.isAlive(m_player)) {
        const ShipControl* control = world.components<ShipControl>().tryGet(m_player);
        const CombatControl* orders = world.components<CombatControl>().tryGet(m_player);
        fine = fine || (control != nullptr &&
                        (control->mode == FlightMode::Manual || control->mode == FlightMode::Pursue));
        fine = fine || (orders != nullptr && orders->targetTrack != 0);
    }
    const SimDuration period = fine ? m_config.tacticalFlightPeriod : m_config.strategicFlightPeriod;
    if (m_simulation->scheduler().system(m_flightSystem).desc.period != period) {
        m_simulation->setSystemPeriod(m_flightSystem, period);
        m_combat.setPeriod(*m_simulation, period);
    }
}

void Sandbox::addJournal(SimTime time, std::string text) {
    m_journal.push_back({time, std::move(text)});
    if (m_journal.size() > kJournalCapacity) {
        m_journal.erase(m_journal.begin());
    }
}

void Sandbox::rebuildPorts(const World& world) {
    m_ports.clear();
    m_stations.clear();
    m_planets.clear();
    m_exitRadius = 0.0;
    const ComponentStore<CelestialBody>& bodies = world.components<CelestialBody>();
    const ComponentStore<OrbitsParent>& orbits = world.components<OrbitsParent>();
    for (usize i = 0; i < bodies.size(); ++i) {
        const EntityId body = bodies.entities()[i];
        const BodyKind kind = bodies.values()[i].kind;
        if (kind == BodyKind::Station) {
            m_ports.push_back(body);
            m_stations.push_back(body);
        } else if (isPlanet(kind)) {
            m_ports.push_back(body);
            m_planets.push_back(body);
            if (const OrbitsParent* orbit = orbits.tryGet(body)) {
                // Apoapsis: a property of the orbit, not of the moment (identical after loading a save).
                m_exitRadius =
                    std::max(m_exitRadius, orbit->orbit.semiMajorAxis * (1.0 + orbit->orbit.eccentricity));
            }
        }
    }
    m_exitRadius *= kExitRadiusFactor;
    m_home = m_stations.empty() ? (m_ports.empty() ? EntityId{} : m_ports.front()) : m_stations.front();
}

std::string Sandbox::nameOf(const World& world, EntityId entity) const {
    if (const CelestialBody* body = world.components<CelestialBody>().tryGet(entity)) {
        return body->name;
    }
    if (const ShipIdentity* ship = world.components<ShipIdentity>().tryGet(entity)) {
        return ship->name;
    }
    return "?";
}

void Sandbox::writeState(BinaryWriter& writer) const {
    writer.io(m_player);
    writer.io(m_playerRespawnAt);
    writer.io(m_lastPlayerHit);
    writer.io(m_combatAlertUntil);
    writer.io(m_nextHaulerSpawn);
    writer.io(m_nextPirateSpawn);
    writer.io(m_systemName);
    writer.io(m_journal);
    writer.io(m_stats);
    writer.io(m_playerCredits);
    writer.io(m_playerPrices);
    writer.io(m_traderPrices);
    writer.io(m_danger);
    writer.io(m_initialStock);
}

void Sandbox::readState(BinaryReader& reader) {
    reader.io(m_player);
    reader.io(m_playerRespawnAt);
    reader.io(m_lastPlayerHit);
    reader.io(m_combatAlertUntil);
    reader.io(m_nextHaulerSpawn);
    reader.io(m_nextPirateSpawn);
    reader.io(m_systemName);
    reader.io(m_journal);
    reader.io(m_stats);
    reader.io(m_playerCredits);
    reader.io(m_playerPrices);
    reader.io(m_traderPrices);
    reader.io(m_danger);
    reader.io(m_initialStock);
    if (reader.ok()) {
        rebuildPorts(m_simulation->world()); // the World is loaded before state blocks
    }
}

} // namespace gx
