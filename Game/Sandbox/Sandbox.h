#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/Kernel/SystemScheduler.h"
#include "Simulation/World/EntityRegistry.h"
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

// First playable vertical slice (M2.1): one generated star system, the player's ship and NPC haulers
// travelling between ports. Everything the player does goes through PilotCommand, so the whole session is
// deterministic, saveable and replayable; it runs identically with or without the graphical client.
struct SandboxConfig {
    u64 seed = 2400;
    u32 haulers = 12;
    // Logical LOD of the flight system: coarse steps while nobody pilots by hand, fine steps otherwise.
    SimDuration strategicFlightPeriod = SimDuration::milliseconds(200);
    SimDuration tacticalFlightPeriod = SimDuration::milliseconds(50);
    // NPC haulers wait this long in port (real-time scale: minutes, not hours).
    SimDuration minDwell = SimDuration::seconds(30);
    SimDuration maxDwell = SimDuration::seconds(180);
    SimDuration sensorScanPeriod = SimDuration::seconds(1);
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

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("haulerDepartures", haulerDepartures);
        ar.io("arrivals", arrivals);
        ar.io("commandsRejected", commandsRejected);
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

    [[nodiscard]] EntityId playerShip() const { return m_player; }
    [[nodiscard]] const std::string& systemName() const { return m_systemName; }
    [[nodiscard]] const std::vector<JournalEntry>& journal() const { return m_journal; }
    [[nodiscard]] const std::vector<EntityId>& ports() const { return m_ports; }
    [[nodiscard]] const SandboxStats& stats() const { return m_stats; }
    [[nodiscard]] SystemId flightSystem() const { return m_flightSystem; }
    [[nodiscard]] const SensorSystem& sensors() const { return m_sensors; }
    [[nodiscard]] const SandboxConfig& config() const { return m_config; }

private:
    void updateHaulers(const TickContext& context);
    void onShipArrived(const ShipArrived& event, const TickContext& context);
    void onHyperspaceTransition(const HyperspaceTransition& event, const TickContext& context);
    void onPilotCommand(const PilotCommand& command, const TickContext& context);
    void onSensorCommand(const SensorCommand& command, const TickContext& context);
    void updateFlightRate(const World& world);
    void addJournal(SimTime time, std::string text);
    void rebuildPorts(const World& world);
    [[nodiscard]] std::string nameOf(const World& world, EntityId entity) const;

    void writeState(BinaryWriter& writer) const;
    void readState(BinaryReader& reader);

    SandboxConfig m_config;
    Simulation* m_simulation = nullptr;
    FlightSystem m_flight;
    SystemId m_flightSystem = kInvalidSystemId;
    SensorSystem m_sensors;
    // Saved state.
    EntityId m_player;
    std::string m_systemName;
    std::vector<JournalEntry> m_journal;
    SandboxStats m_stats;
    // Derived from the World (rebuilt after loading).
    std::vector<EntityId> m_ports;
};

} // namespace gx
