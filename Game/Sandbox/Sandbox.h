#pragma once

#include "Engine/Core/Random.h"
#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/Economy/Economy.h"
#include "Simulation/Kernel/SystemScheduler.h"
#include "Simulation/World/EntityRegistry.h"
#include "Space/Combat/Combat.h"
#include "Space/Sensors/Sensors.h"
#include "Space/Ships/Flight.h"
#include "Space/Ships/Ship.h"

#include <string>
#include <vector>

namespace gx {

class BinaryReader;
class BinaryWriter;
class Simulation;
class World;
struct TickContext;

// First playable vertical slice (M2): one generated star system, the player's ship, NPC haulers travelling
// between ports and pirates hunting them. Everything the player does goes through commands, so the whole
// session is deterministic, saveable and replayable; it runs identically with or without the graphical
// client.
struct SandboxConfig {
    u64 seed = 2400;
    u32 haulers = 12;
    u32 pirates = 3;
    // Logical LOD of the flight and combat systems: coarse steps unless the player flies by hand or fights.
    SimDuration strategicFlightPeriod = SimDuration::milliseconds(200);
    SimDuration tacticalFlightPeriod = SimDuration::milliseconds(50);
    // NPC haulers wait this long in port (real-time scale: minutes, not hours).
    SimDuration minDwell = SimDuration::seconds(30);
    SimDuration maxDwell = SimDuration::seconds(180);
    SimDuration sensorScanPeriod = SimDuration::seconds(1);
    // Losses are replaced by newcomers after these delays (the system is not a closed box).
    SimDuration haulerRespawnDelay = SimDuration::seconds(60);
    SimDuration pirateRespawnDelay = SimDuration::minutes(3);
    SimDuration playerRespawnDelay = SimDuration::seconds(10);
    // Tactical steps last this long after the player was last hit.
    SimDuration combatAlert = SimDuration::seconds(30);
    SimDuration economyPeriod = SimDuration::seconds(10);
};

// Player input. Validated by the handler (never trusted).
struct PilotCommand {
    EntityId ship;
    FlightMode mode = FlightMode::Stop;
    EntityId target; // Approach
    Vec3d point;     // MoveTo
    Vec3d thrust;    // Manual, length <= 1

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("ship", ship);
        ar.io("mode", mode);
        ar.io("target", target);
        ar.io("point", point);
        ar.io("thrust", thrust);
    }
};

// Player input: radar and transponder switches. Validated by the handler.
struct SensorCommand {
    EntityId ship;
    bool activeOn = false;
    bool transponderOn = true;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("ship", ship);
        ar.io("activeOn", activeOn);
        ar.io("transponderOn", transponderOn);
    }
};

// Player input: fire orders and pursuit of a sensor track (track 0 with fire = false: cease fire).
struct EngageCommand {
    EntityId ship;
    u32 track = 0;
    bool fire = false;
    bool pursue = false;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("ship", ship);
        ar.io("track", track);
        ar.io("fire", fire);
        ar.io("pursue", pursue);
    }
};

// Player input: buy (tonnes > 0) or sell (tonnes < 0) a good at the port the ship is docked at.
struct TradeCommand {
    EntityId ship;
    GoodId good = 0;
    i32 tonnes = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("ship", ship);
        ar.io("good", good);
        ar.io("tonnes", tonnes);
    }
};

// NPC hauler behaviour state: wait at a port, then fly to another one.
struct HaulerBrain {
    SimTime departAt;
    EntityId lastPort;
    u32 trips = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("departAt", departAt);
        ar.io("lastPort", lastPort);
        ar.io("trips", trips);
    }
};

enum class PirateState : u8 { Lurking, Hunting, Leaving, Count };

[[nodiscard]] const char* toString(PirateState state);

// NPC raider behaviour state. Decisions use only the pirate faction's sensor picture (ADR-025).
struct PirateBrain {
    PirateState state = PirateState::Lurking;
    SimTime nextMove;    // Lurking: when to move to another ambush point; Hunting: when to give up
    u32 decisions = 0;   // key for the per-decision random streams
    u32 ignoreTrack = 0; // a prey given up on

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("state", state);
        ar.io("nextMove", nextMove);
        ar.io("decisions", decisions);
        ar.io("ignoreTrack", ignoreTrack);
    }
};

struct JournalEntry {
    SimTime time;
    std::string text;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("time", time);
        ar.io("text", text);
    }
};

struct SandboxStats {
    u64 haulerDepartures = 0;
    u64 arrivals = 0;
    u64 commandsRejected = 0;
    u64 haulersLost = 0;
    u64 piratesLost = 0;
    u64 piratesLeft = 0;
    u64 playerDeaths = 0;
    u64 hunts = 0;
    u64 spawns = 0;
    u64 haulerTrades = 0;     // loads bought by haulers
    u64 tonnesDelivered = 0;  // sold by haulers at their destination
    u64 explorationTrips = 0; // haulers flying empty to refresh their prices
    u64 repositionTrips = 0;  // haulers flying empty to where a known bargain is
    u64 playerTrades = 0;
    std::vector<u64> cargoLost; // tonnes by good, lost with destroyed ships

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("haulerDepartures", haulerDepartures);
        ar.io("arrivals", arrivals);
        ar.io("commandsRejected", commandsRejected);
        ar.io("haulersLost", haulersLost);
        ar.io("piratesLost", piratesLost);
        ar.io("piratesLeft", piratesLeft);
        ar.io("playerDeaths", playerDeaths);
        ar.io("hunts", hunts);
        ar.io("spawns", spawns);
        ar.io("haulerTrades", haulerTrades);
        ar.io("tonnesDelivered", tonnesDelivered);
        ar.io("explorationTrips", explorationTrips);
        ar.io("repositionTrips", repositionTrips);
        ar.io("playerTrades", playerTrades);
        ar.io("cargoLost", cargoLost);
    }
};

// What the traders remember about losses near a port (decays with time).
struct PortDanger {
    EntityId port;
    f64 level = 0.0;
    SimTime updated;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("port", port);
        ar.io("level", level);
        ar.io("updated", updated);
    }
};

class Sandbox {
public:
    static constexpr usize kJournalCapacity = 200;

    explicit Sandbox(const SandboxConfig& config);
    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    // Registers every type, system, command and state block. Required for new games and for loading.
    void install(Simulation& simulation);
    // New game only: generates the star system and spawns the ships (skip when loading a save).
    void populate(Simulation& simulation);

    // Invalid between the destruction of the player's ship and its replacement.
    [[nodiscard]] EntityId playerShip() const { return m_player; }
    [[nodiscard]] SimTime playerRespawnAt() const { return m_playerRespawnAt; }
    [[nodiscard]] EntityId homePort() const { return m_home; }
    [[nodiscard]] const std::string& systemName() const { return m_systemName; }
    [[nodiscard]] const std::vector<JournalEntry>& journal() const { return m_journal; }
    [[nodiscard]] const std::vector<EntityId>& ports() const { return m_ports; }
    [[nodiscard]] const SandboxStats& stats() const { return m_stats; }
    [[nodiscard]] SystemId flightSystem() const { return m_flightSystem; }
    [[nodiscard]] const SensorSystem& sensors() const { return m_sensors; }
    [[nodiscard]] const CombatSystem& combat() const { return m_combat; }
    [[nodiscard]] const EconomySystem& economy() const { return m_economy; }
    // Price knowledge of the player and of the independent traders (their shared network).
    [[nodiscard]] const PriceBook& playerPrices() const { return m_playerPrices; }
    [[nodiscard]] const PriceBook& traderPrices() const { return m_traderPrices; }
    [[nodiscard]] f64 danger(EntityId port, SimTime now) const;
    // Tonnes of each good in the markets when the game started (conservation checks).
    [[nodiscard]] const std::vector<f64>& initialStock() const { return m_initialStock; }
    // The port whose market the ship is docked at (keeping station there), or invalid.
    [[nodiscard]] EntityId dockedPort(const World& world, EntityId ship) const;
    [[nodiscard]] const SandboxConfig& config() const { return m_config; }
    [[nodiscard]] bool tactical() const;

private:
    EntityId spawnShip(World& world, SimTime now, Rng& rng, std::string name, u32 faction, u32 shipClass,
                       EntityId port);
    EntityId spawnHauler(World& world, SimTime now, Rng& rng, u32 serial);
    EntityId spawnPirate(World& world, SimTime now, Rng& rng, u32 serial);
    void setupMarkets(World& world);
    void planHaulerTrip(World& world, EntityId ship, HaulerBrain& brain, ShipControl& control, SimTime now,
                        Rng& rng);
    void sellCargo(World& world, EntityId ship, EntityId port, SimTime now);
    void addInflight(EntityId port, GoodId good, f64 tonnes);
    void addDanger(EntityId port, SimTime now);
    void onTradeCommand(const TradeCommand& command, const TickContext& context);
    void updateHaulers(const TickContext& context);
    void updatePirates(const TickContext& context);
    void updateUpkeep(const TickContext& context);
    void onShipArrived(const ShipArrived& event, const TickContext& context);
    void onHyperspaceTransition(const HyperspaceTransition& event, const TickContext& context);
    void onShipDamaged(const ShipDamaged& event, const TickContext& context);
    void onShipDestroyed(const ShipDestroyed& event, const TickContext& context);
    void onPilotCommand(const PilotCommand& command, const TickContext& context);
    void onSensorCommand(const SensorCommand& command, const TickContext& context);
    void onEngageCommand(const EngageCommand& command, const TickContext& context);
    void updateFlightRate(const World& world, SimTime now);
    void addJournal(SimTime time, std::string text);
    void rebuildPorts(const World& world);
    [[nodiscard]] std::string nameOf(const World& world, EntityId entity) const;
    [[nodiscard]] EntityId nearestStation(const World& world, const Vec3d& position, SimTime now) const;
    [[nodiscard]] Vec3d ambushPoint(const World& world, EntityId planet, SimTime now, Rng& rng) const;

    void writeState(BinaryWriter& writer) const;
    void readState(BinaryReader& reader);

    SandboxConfig m_config;
    Simulation* m_simulation = nullptr;
    FlightSystem m_flight;
    SystemId m_flightSystem = kInvalidSystemId;
    SensorSystem m_sensors;
    CombatSystem m_combat;
    EconomySystem m_economy;
    // Saved state.
    EntityId m_player;
    SimTime m_playerRespawnAt;
    SimTime m_lastPlayerHit;
    SimTime m_combatAlertUntil;
    SimTime m_nextHaulerSpawn;
    SimTime m_nextPirateSpawn;
    std::string m_systemName;
    std::vector<JournalEntry> m_journal;
    SandboxStats m_stats;
    i64 m_playerCredits = 0; // carried over to a replacement ship
    PriceBook m_playerPrices;
    PriceBook m_traderPrices;
    std::vector<PortDanger> m_danger;
    std::vector<f64> m_initialStock;
    // Derived from the World (rebuilt after loading, or on every Game.Haulers run).
    struct Delivery {
        EntityId port;
        GoodId good = 0;
        f64 tonnes = 0.0;
    };
    std::vector<Delivery> m_inflight; // cargo the traders' ships are carrying to each port
    std::vector<EntityId> m_ports;
    std::vector<EntityId> m_stations;
    std::vector<EntityId> m_planets;
    EntityId m_home;
    f64 m_exitRadius = 0.0; // beyond every planet's orbit: raiders leave the system through it
};

} // namespace gx
