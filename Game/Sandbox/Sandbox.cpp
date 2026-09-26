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

#include <cmath>
#include <format>

namespace gx {
namespace {

constexpr u64 kSpawnStream = fnv1a64("sandbox.spawn");
constexpr u64 kRouteStream = fnv1a64("sandbox.hauler.route");
constexpr u64 kDwellStream = fnv1a64("sandbox.hauler.dwell");

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

} // namespace

Sandbox::Sandbox(const SandboxConfig& config) : m_config(config) {}

void Sandbox::install(Simulation& simulation) {
    m_simulation = &simulation;
    registerSpaceTypes(simulation);
    SensorSystem::registerTypes(simulation);
    simulation.world().registerComponent<HaulerBrain>("Game.HaulerBrain");

    simulation.events().channel<ShipArrived>().subscribe(
        [this](const ShipArrived& event, const TickContext& context) { onShipArrived(event, context); });
    simulation.events().channel<HyperspaceTransition>().subscribe(
        [this](const HyperspaceTransition& event, const TickContext& context) {
            onHyperspaceTransition(event, context);
        });
    simulation.commands().registerCommand<PilotCommand>(
        "Game.Pilot", [this](const PilotCommand& command, const TickContext& context) {
            onPilotCommand(command, context);
        });

    // Behaviour runs before flight in the same phase, so new orders fly in the same step.
    simulation.addSystem({"Game.Haulers",
                          TickPhase::Simulation,
                          SimDuration::minutes(1),
                          {},
                          [this](const TickContext& context) { updateHaulers(context); }});
    m_flightSystem = m_flight.install(simulation, m_config.strategicFlightPeriod);
    m_sensors.install(simulation, m_config.sensorScanPeriod); // after flight: scans see this step's motion
    simulation.commands().registerCommand<SensorCommand>(
        "Game.Sensors", [this](const SensorCommand& command, const TickContext& context) {
            onSensorCommand(command, context);
        });

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
    GX_CHECK(!m_ports.empty(), "the generated system has no ports");

    EntityId home = m_ports.front();
    for (const EntityId port : m_ports) {
        if (world.components<CelestialBody>().get(port).kind == BodyKind::Station) {
            home = port;
            break;
        }
    }

    Rng rng = Rng::forStream(m_config.seed, kSpawnStream);
    const auto spawnShip = [&](std::string name, u32 faction, u32 shipClass, EntityId port) {
        const OrbitState portState = bodyStateAt(world, port, now);
        const f64 standoff = standoffDistance(world, port);
        f64 dx = 0.0;
        f64 dy = 0.0;
        f64 lengthSq = 0.0;
        do {
            dx = rng.uniform(-1.0, 1.0);
            dy = rng.uniform(-1.0, 1.0);
            lengthSq = dx * dx + dy * dy;
        } while (lengthSq < 1e-4 || lengthSq > 1.0);
        const Vec3d offset = Vec3d{dx, dy, 0.0} * (standoff / std::sqrt(lengthSq));

        const content::ShipClassDef& shipDef = content::kShipClasses[shipClass];
        const EntityId ship = world.createEntity();
        world.components<Kinematics>().add(ship, {portState.position + offset, portState.velocity, {}});
        world.components<ShipDrive>().add(ship, {shipDef.maxAcceleration, shipDef.cruiseSpeed,
                                                 shipDef.hyperspaceSpeed, shipDef.hyperspaceChargeTime});
        ShipControl control;
        control.mode = FlightMode::Approach; // keeping station at the port
        control.target = port;
        control.standoff = standoff;
        control.arrived = true;
        world.components<ShipControl>().add(ship, control);
        world.components<ShipIdentity>().add(ship, {std::move(name), faction, shipClass});
        const content::SensorDef& sensorDef = content::kShipSensors[shipClass];
        world.components<SensorSuite>().add(
            ship, {sensorDef.passiveSensitivity, sensorDef.activeStrength, false, true});
        world.components<SignatureProfile>().add(
            ship, {sensorDef.baseEmission, sensorDef.driveEmission, sensorDef.crossSection});
        return ship;
    };

    m_player = spawnShip(content::kPlayerShipName, content::kFactionPlayer, content::kShipClassCourier, home);
    for (u32 i = 0; i < m_config.haulers; ++i) {
        const EntityId port = m_ports[rng.uniformU32(static_cast<u32>(m_ports.size()))];
        std::string name = std::format("{}-{}", content::kHaulerNames[i % content::kHaulerNames.size()],
                                       10 + rng.uniformU32(90));
        const EntityId ship =
            spawnShip(std::move(name), content::kFactionIndependent, content::kShipClassHauler, port);
        const auto maxDwellMs = static_cast<u32>(m_config.maxDwell.count() / 1000);
        world.components<HaulerBrain>().add(
            ship, {now + SimDuration::milliseconds(rng.uniformU32(maxDwellMs)), port, 0});
    }
    addJournal(now, std::format("Comienza la partida en el sistema {}.", m_systemName));
}

void Sandbox::updateHaulers(const TickContext& context) {
    if (m_ports.size() < 2) {
        return;
    }
    World& world = context.world;
    ComponentStore<HaulerBrain>& brains = world.components<HaulerBrain>();
    ComponentStore<ShipControl>& controls = world.components<ShipControl>();
    const auto portCount = static_cast<u32>(m_ports.size());
    for (usize i = 0; i < brains.size(); ++i) {
        const EntityId ship = brains.entities()[i];
        HaulerBrain& brain = brains.values()[i];
        ShipControl& control = controls.get(ship);
        const bool docked = control.arrived || control.mode == FlightMode::Stop;
        if (!docked || context.now < brain.departAt) {
            continue;
        }
        // Next port, never the one it is leaving. Randomness keyed by (ship, trip): order-independent.
        Rng rng = Rng::forStream(context.worldSeed, hashCombine(kRouteStream, entityKey(ship)), brain.trips);
        u32 lastIndex = portCount;
        for (u32 p = 0; p < portCount; ++p) {
            if (m_ports[p] == brain.lastPort) {
                lastIndex = p;
            }
        }
        u32 next = rng.uniformU32(lastIndex < portCount ? portCount - 1 : portCount);
        if (lastIndex < portCount && next >= lastIndex) {
            ++next;
        }
        const EntityId destination = m_ports[next];
        control.mode = FlightMode::Approach;
        control.target = destination;
        control.standoff = standoffDistance(world, destination);
        control.arrived = false;
        ++brain.trips;
        ++m_stats.haulerDepartures;
    }
}

void Sandbox::onShipArrived(const ShipArrived& event, const TickContext& context) {
    World& world = context.world;
    ++m_stats.arrivals;
    if (HaulerBrain* brain = world.components<HaulerBrain>().tryGet(event.ship)) {
        brain->lastPort = event.target;
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
        reject("invalid flight mode");
        return;
    }

    ShipControl& control = world.components<ShipControl>().get(command.ship);
    control.mode = command.mode;
    control.arrived = false;
    control.target = command.mode == FlightMode::Approach ? command.target : EntityId{};
    control.point = command.mode == FlightMode::MoveTo ? command.point : Vec3d{};
    control.manualThrust = command.mode == FlightMode::Manual ? clampToUnit(command.thrust) : Vec3d{};
    control.standoff = command.mode == FlightMode::Approach ? standoffDistance(world, command.target) : 0.0;

    if (command.mode == FlightMode::Approach) {
        addJournal(context.now, std::format("Rumbo a {}.", nameOf(world, command.target)));
    } else if (command.mode == FlightMode::MoveTo) {
        addJournal(context.now, "Rumbo a un punto del espacio.");
    } else if (command.mode == FlightMode::Stop) {
        addJournal(context.now, "Deteniendo la nave.");
    }
    updateFlightRate(world);
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

void Sandbox::updateFlightRate(const World& world) {
    // Fine flight steps only while the player flies by hand: responsive controls without paying for
    // 10 Hz integration during long autopilot trips. Requests during a step apply at its end, in order.
    const ShipControl* control = world.components<ShipControl>().tryGet(m_player);
    const bool manual = control != nullptr && control->mode == FlightMode::Manual;
    m_simulation->setSystemPeriod(m_flightSystem,
                                  manual ? m_config.tacticalFlightPeriod : m_config.strategicFlightPeriod);
}

void Sandbox::addJournal(SimTime time, std::string text) {
    m_journal.push_back({time, std::move(text)});
    if (m_journal.size() > kJournalCapacity) {
        m_journal.erase(m_journal.begin());
    }
}

void Sandbox::rebuildPorts(const World& world) {
    m_ports.clear();
    const ComponentStore<CelestialBody>& bodies = world.components<CelestialBody>();
    for (usize i = 0; i < bodies.size(); ++i) {
        const BodyKind kind = bodies.values()[i].kind;
        if (kind == BodyKind::Station || isPlanet(kind)) {
            m_ports.push_back(bodies.entities()[i]);
        }
    }
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
    writer.io(m_systemName);
    writer.io(m_journal);
    writer.io(m_stats);
}

void Sandbox::readState(BinaryReader& reader) {
    reader.io(m_player);
    reader.io(m_systemName);
    reader.io(m_journal);
    reader.io(m_stats);
    if (reader.ok()) {
        rebuildPorts(m_simulation->world()); // the World is loaded before state blocks
    }
}

} // namespace gx
