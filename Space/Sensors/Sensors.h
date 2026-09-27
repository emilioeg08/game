#pragma once

#include "Engine/Core/Random.h"
#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/Kernel/SystemScheduler.h"
#include "Simulation/World/EntityRegistry.h"
#include "Space/Ships/Ship.h"

#include <span>
#include <vector>

// Sensors (prompt §14, ADR-023): nobody is omniscient. Each faction builds its own picture of the other
// factions' ships from what its sensors detect; the player's UI and the AI only ever read such pictures.
//
//   passive  SNR = emission * sensitivity / d^2        silent, imprecise, depends on what the target emits
//   active   SNR = strength * crossSection / d^4       precise and identifies, but the radar emission itself
//                                                      is detectable far beyond the radar's own range
//   transponder: broadcasts identity and position within kTransponderRange (switchable)
//
// Detection is certain above SNR 1 and probabilistic between 0.25 and 1. Estimates carry noise that shrinks
// with SNR; contacts not refreshed for kContactTimeout are lost; noise occasionally produces ghost contacts.
// All randomness comes from per-(faction, target, scan) streams: deterministic for any thread count.
namespace gx {

class BinaryReader;
class BinaryWriter;
class Simulation;
class World;
struct TickContext;

inline constexpr f64 kTransponderRange = 1.495978707e11; // 1 AU
inline constexpr f64 kActiveSensorEmission = 1e8;        // radar pulses make the emitter bright
inline constexpr f64 kChargingEmission = 1e9;
inline constexpr f64 kHyperspaceEmission = 1e12; // visible across most of a system
inline constexpr SimDuration kContactTimeout = SimDuration::seconds(20);
inline constexpr SimDuration kGhostLifetime = SimDuration::seconds(4);
inline constexpr f64 kPassiveAngularError = 0.01; // 1-sigma position error as a fraction of range at SNR 1
inline constexpr f64 kActiveAngularError = 0.0005;

struct SensorSuite {
    f64 passiveSensitivity = 0.0; // m^2 per emission unit
    f64 activeStrength = 0.0;     // 0: no radar
    bool activeOn = false;
    bool transponderOn = true;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("passiveSensitivity", passiveSensitivity);
        ar.io("activeStrength", activeStrength);
        ar.io("activeOn", activeOn);
        ar.io("transponderOn", transponderOn);
    }
};

// How visible a hull is: what it emits at rest and at full thrust, and how well it reflects radar.
struct SignatureProfile {
    f64 baseEmission = 0.0;
    f64 driveEmission = 0.0; // added in proportion to the thrust in use
    f64 crossSection = 0.0;  // m^2

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("baseEmission", baseEmission);
        ar.io("driveEmission", driveEmission);
        ar.io("crossSection", crossSection);
    }
};

enum class ContactLevel : u8 {
    Unknown,    // something is there
    Classified, // hull class known
    Identified, // name and faction known
    Count
};

[[nodiscard]] const char* toString(ContactLevel level);

struct SensorContact {
    u32 trackId = 0;
    EntityId target;       // internal track association; invalid for ghosts. Never shown unless Identified.
    Vec3d position;        // estimate at lastSeen
    Vec3d velocity;        // estimate
    f64 uncertainty = 0.0; // m, 1-sigma of the position estimate
    SimTime firstSeen;
    SimTime lastSeen;
    ContactLevel level = ContactLevel::Unknown;
    u32 shipClass = 0; // valid from Classified
    u32 faction = 0;   // valid when Identified
    bool ghost = false;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("trackId", trackId);
        ar.io("target", target);
        ar.io("position", position);
        ar.io("velocity", velocity);
        ar.io("uncertainty", uncertainty);
        ar.io("firstSeen", firstSeen);
        ar.io("lastSeen", lastSeen);
        ar.io("level", level);
        ar.io("shipClass", shipClass);
        ar.io("faction", faction);
        ar.io("ghost", ghost);
    }
};

// What one faction knows about everybody else's ships. Contacts are kept sorted by trackId.
struct FactionPicture {
    u32 faction = 0;
    u32 nextTrackId = 1;
    std::vector<SensorContact> contacts;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("faction", faction);
        ar.io("nextTrackId", nextTrackId);
        ar.io("contacts", contacts);
    }
};

// Current emission of a ship: base + thrust share + radar + hyperspace drive activity.
[[nodiscard]] f64 shipEmission(const SignatureProfile& profile, const ShipDrive& drive,
                               const Kinematics& kinematics, const ShipControl& control,
                               const SensorSuite& sensors);
[[nodiscard]] f64 passiveSnr(f64 emission, f64 sensitivity, f64 distance);
[[nodiscard]] f64 activeSnr(f64 strength, f64 crossSection, f64 distance);
// Probability that a single scan detects a target with this SNR (1 above 1, linear from 0.25 to 1).
[[nodiscard]] f64 detectionProbability(f64 snr);
// Distance at which a passive sensor of `sensitivity` detects `emission` with certainty.
[[nodiscard]] f64 passiveDetectionRange(f64 emission, f64 sensitivity);
// 1-sigma position error of the best of a passive and an active reading (infinity if neither registers).
[[nodiscard]] f64 measurementSigma(f64 distance, f64 passiveSnr, f64 activeSnr);
// Per-axis measurement noise with the given standard deviation.
[[nodiscard]] Vec3d sensorNoise(Rng& rng, f64 sigma);

class SensorSystem {
public:
    // Registers the sensor components (call once, like registerSpaceTypes).
    static void registerTypes(Simulation& simulation);

    // Registers the scan system (period = scan interval) and the "Space.Sensors" state block.
    SystemId install(Simulation& simulation, SimDuration scanPeriod);
    void update(const TickContext& context);

    // Picture of a faction (empty if it has never seen anything).
    [[nodiscard]] const FactionPicture& picture(u32 faction) const;
    [[nodiscard]] const SensorContact* findContact(u32 faction, u32 trackId) const;

private:
    FactionPicture& pictureFor(u32 faction);
    void writeState(BinaryWriter& writer) const;
    void readState(BinaryReader& reader);

    std::vector<FactionPicture> m_pictures; // sorted by faction
    FactionPicture m_empty;
};

} // namespace gx
