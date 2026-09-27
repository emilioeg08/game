#include "Space/Sensors/Sensors.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Serialization/Binary.h"
#include "Simulation/Kernel/Simulation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace gx {
namespace {

constexpr u64 kScanStream = fnv1a64("space.sensors.scan");
constexpr u64 kGhostStream = fnv1a64("space.sensors.ghost");
constexpr f64 kGhostChance = 0.04; // per faction per scan
constexpr f64 kClassifySnr = 4.0;
constexpr f64 kPassiveIdentifySnr = 25.0;
constexpr f64 kActiveIdentifySnr = 4.0;
constexpr f64 kTransponderError = 1'000.0; // m: transponders report their own position

u64 entityKey(EntityId entity) {
    return (static_cast<u64>(entity.generation) << 32) | entity.index;
}

struct ShipSnapshot {
    EntityId id;
    u32 faction = 0;
    u32 shipClass = 0;
    Vec3d position;
    Vec3d velocity;
    f64 emission = 0.0;
    f64 crossSection = 0.0;
    const SensorSuite* sensors = nullptr; // null: cannot observe
    bool transponder = false;
};

} // namespace

const char* toString(ContactLevel level) {
    switch (level) {
    case ContactLevel::Unknown:
        return "Unknown";
    case ContactLevel::Classified:
        return "Classified";
    case ContactLevel::Identified:
        return "Identified";
    case ContactLevel::Count:
        break;
    }
    return "?";
}

f64 shipEmission(const SignatureProfile& profile, const ShipDrive& drive, const Kinematics& kinematics,
                 const ShipControl& control, const SensorSuite& sensors) {
    f64 emission = profile.baseEmission;
    if (drive.maxAcceleration > 0.0) {
        emission +=
            profile.driveEmission * std::min(1.0, length(kinematics.acceleration) / drive.maxAcceleration);
    }
    if (sensors.activeOn && sensors.activeStrength > 0.0) {
        emission += kActiveSensorEmission;
    }
    if (control.phase == DrivePhase::Hyperspace) {
        emission += kHyperspaceEmission;
    } else if (control.phase == DrivePhase::Charging) {
        emission += kChargingEmission;
    }
    return emission;
}

f64 passiveSnr(f64 emission, f64 sensitivity, f64 distance) {
    const f64 d = std::max(distance, 1.0);
    return emission * sensitivity / (d * d);
}

f64 activeSnr(f64 strength, f64 crossSection, f64 distance) {
    const f64 dSquared = std::max(distance, 1.0) * std::max(distance, 1.0);
    return strength * crossSection / (dSquared * dSquared);
}

f64 detectionProbability(f64 snr) {
    return std::clamp((snr - 0.25) / 0.75, 0.0, 1.0);
}

f64 passiveDetectionRange(f64 emission, f64 sensitivity) {
    return std::sqrt(emission * sensitivity);
}

f64 measurementSigma(f64 distance, f64 passive, f64 active) {
    f64 sigma = std::numeric_limits<f64>::infinity();
    if (passive >= 0.25) {
        sigma = std::min(sigma, distance * kPassiveAngularError / std::sqrt(passive));
    }
    if (active >= 0.25) {
        sigma = std::min(sigma, distance * kActiveAngularError / std::sqrt(active));
    }
    return sigma;
}

Vec3d sensorNoise(Rng& rng, f64 sigma) {
    const f64 halfWidth = sigma * 1.7320508075688772; // uniform on [-a, a] has sigma a/sqrt(3)
    return {rng.uniform(-halfWidth, halfWidth), rng.uniform(-halfWidth, halfWidth),
            rng.uniform(-halfWidth, halfWidth)};
}

void SensorSystem::registerTypes(Simulation& simulation) {
    simulation.world().registerComponent<SensorSuite>("Space.SensorSuite");
    simulation.world().registerComponent<SignatureProfile>("Space.SignatureProfile");
}

SystemId SensorSystem::install(Simulation& simulation, SimDuration scanPeriod) {
    simulation.addStateBlock(
        "Space.Sensors", [this](BinaryWriter& writer) { writeState(writer); },
        [this](BinaryReader& reader) { readState(reader); });
    return simulation.addSystem(
        {"Space.Sensors", TickPhase::Simulation, scanPeriod, {}, [this](const TickContext& context) {
             update(context);
         }});
}

const FactionPicture& SensorSystem::picture(u32 faction) const {
    for (const FactionPicture& picture : m_pictures) {
        if (picture.faction == faction) {
            return picture;
        }
    }
    return m_empty;
}

const SensorContact* SensorSystem::findContact(u32 faction, u32 trackId) const {
    // Contacts are sorted by trackId (new tracks are appended with growing ids; losses keep the order).
    const std::vector<SensorContact>& contacts = picture(faction).contacts;
    const auto it = std::lower_bound(contacts.begin(), contacts.end(), trackId,
                                     [](const SensorContact& c, u32 id) { return c.trackId < id; });
    return it != contacts.end() && it->trackId == trackId ? &*it : nullptr;
}

FactionPicture& SensorSystem::pictureFor(u32 faction) {
    const auto it = std::lower_bound(m_pictures.begin(), m_pictures.end(), faction,
                                     [](const FactionPicture& p, u32 f) { return p.faction < f; });
    if (it != m_pictures.end() && it->faction == faction) {
        return *it;
    }
    FactionPicture created;
    created.faction = faction;
    return *m_pictures.insert(it, std::move(created));
}

void SensorSystem::update(const TickContext& context) {
    const World& world = context.world;
    const ComponentStore<ShipIdentity>& identities = world.components<ShipIdentity>();
    const ComponentStore<Kinematics>& kinematics = world.components<Kinematics>();
    const ComponentStore<ShipControl>& controls = world.components<ShipControl>();
    const ComponentStore<ShipDrive>& drives = world.components<ShipDrive>();
    const ComponentStore<SensorSuite>& suites = world.components<SensorSuite>();
    const ComponentStore<SignatureProfile>& profiles = world.components<SignatureProfile>();

    // What every ship looks like right now (dense order: deterministic).
    std::vector<ShipSnapshot> ships;
    ships.reserve(identities.size());
    std::vector<u32> observerFactions;
    for (usize i = 0; i < identities.size(); ++i) {
        const EntityId id = identities.entities()[i];
        const Kinematics* state = kinematics.tryGet(id);
        const ShipControl* control = controls.tryGet(id);
        const ShipDrive* drive = drives.tryGet(id);
        const SignatureProfile* profile = profiles.tryGet(id);
        const SensorSuite* suite = suites.tryGet(id);
        if (state == nullptr || control == nullptr || drive == nullptr || profile == nullptr) {
            continue;
        }
        static const SensorSuite kNoSensors{};
        ShipSnapshot ship;
        ship.id = id;
        ship.faction = identities.values()[i].faction;
        ship.shipClass = identities.values()[i].shipClass;
        ship.position = state->position;
        ship.velocity = state->velocity;
        ship.emission =
            shipEmission(*profile, *drive, *state, *control, suite != nullptr ? *suite : kNoSensors);
        ship.crossSection = profile->crossSection;
        ship.sensors = suite;
        ship.transponder = suite != nullptr && suite->transponderOn;
        ships.push_back(ship);
        if (suite != nullptr) {
            observerFactions.push_back(ship.faction);
        }
    }
    std::sort(observerFactions.begin(), observerFactions.end());
    observerFactions.erase(std::unique(observerFactions.begin(), observerFactions.end()),
                           observerFactions.end());

    // Observers of each faction, so a scan costs (observers x targets) instead of (ships x targets).
    std::vector<std::vector<u32>> observersByFaction(observerFactions.size());
    for (usize i = 0; i < ships.size(); ++i) {
        if (ships[i].sensors != nullptr) {
            const auto slot = static_cast<usize>(
                std::lower_bound(observerFactions.begin(), observerFactions.end(), ships[i].faction) -
                observerFactions.begin());
            observersByFaction[slot].push_back(static_cast<u32>(i));
        }
    }

    std::unordered_map<u64, usize> contactByTarget; // lookup only: iteration order never depends on it
    for (usize factionSlot = 0; factionSlot < observerFactions.size(); ++factionSlot) {
        const u32 faction = observerFactions[factionSlot];
        const std::vector<u32>& observers = observersByFaction[factionSlot];
        FactionPicture& picture = pictureFor(faction);
        const u64 factionStream = hashCombine(kScanStream, faction);
        contactByTarget.clear();
        for (usize c = 0; c < picture.contacts.size(); ++c) {
            if (!picture.contacts[c].ghost) {
                contactByTarget.emplace(entityKey(picture.contacts[c].target), c);
            }
        }

        for (const ShipSnapshot& target : ships) {
            if (target.faction == faction) {
                continue; // own ships are known through the fleet, not through sensors
            }
            // Best reading over every observer of the faction.
            f64 bestPassive = 0.0;
            f64 bestActive = 0.0;
            bool transponderHeard = false;
            f64 bestSigma = std::numeric_limits<f64>::infinity();
            for (const u32 observerIndex : observers) {
                const ShipSnapshot& observer = ships[observerIndex];
                const f64 distance = length(target.position - observer.position);
                const f64 passive =
                    passiveSnr(target.emission, observer.sensors->passiveSensitivity, distance);
                const f64 active =
                    observer.sensors->activeOn && observer.sensors->activeStrength > 0.0
                        ? activeSnr(observer.sensors->activeStrength, target.crossSection, distance)
                        : 0.0;
                bestPassive = std::max(bestPassive, passive);
                bestActive = std::max(bestActive, active);
                bestSigma = std::min(bestSigma, measurementSigma(distance, passive, active));
                if (target.transponder && distance <= kTransponderRange) {
                    transponderHeard = true;
                    bestSigma = std::min(bestSigma, kTransponderError);
                }
            }

            Rng rng = Rng::forStream(context.worldSeed, hashCombine(factionStream, entityKey(target.id)),
                                     context.runIndex);
            const f64 snr = std::max(bestPassive, bestActive);
            const bool detected = transponderHeard || rng.nextF64() < detectionProbability(snr);
            if (!detected) {
                continue;
            }
            ContactLevel level = ContactLevel::Unknown;
            if (transponderHeard || bestActive >= kActiveIdentifySnr || bestPassive >= kPassiveIdentifySnr) {
                level = ContactLevel::Identified;
            } else if (bestPassive >= kClassifySnr || bestActive >= 1.0) {
                level = ContactLevel::Classified;
            }

            usize contactIndex = 0;
            if (const auto known = contactByTarget.find(entityKey(target.id));
                known != contactByTarget.end()) {
                contactIndex = known->second;
            } else {
                SensorContact created;
                created.trackId = picture.nextTrackId++;
                created.target = target.id;
                created.firstSeen = context.now;
                picture.contacts.push_back(created); // trackIds grow: the list stays sorted
                contactIndex = picture.contacts.size() - 1;
                contactByTarget.emplace(entityKey(target.id), contactIndex);
            }
            SensorContact* contact = &picture.contacts[contactIndex];
            contact->position = target.position + sensorNoise(rng, bestSigma);
            contact->velocity = target.velocity + sensorNoise(rng, bestSigma / 10.0);
            contact->uncertainty = bestSigma;
            contact->lastSeen = context.now;
            contact->level = std::max(contact->level, level); // what has been learned is not forgotten
            if (contact->level >= ContactLevel::Classified) {
                contact->shipClass = target.shipClass;
            }
            if (contact->level == ContactLevel::Identified) {
                contact->faction = target.faction;
            }
        }

        // Sensor noise: now and then a false contact, somewhere a real one could plausibly be.
        Rng ghostRng =
            Rng::forStream(context.worldSeed, hashCombine(kGhostStream, faction), context.runIndex);
        if (ghostRng.chance(kGhostChance)) {
            const ShipSnapshot* observer = observers.empty() ? nullptr : &ships[observers.front()];
            if (observer != nullptr) {
                const f64 range = passiveDetectionRange(1e6, observer->sensors->passiveSensitivity) *
                                  ghostRng.uniform(0.2, 1.0);
                Vec3d direction{ghostRng.uniform(-1.0, 1.0), ghostRng.uniform(-1.0, 1.0),
                                ghostRng.uniform(-0.1, 0.1)};
                direction = direction / std::max(length(direction), 1e-9);
                SensorContact ghost;
                ghost.trackId = picture.nextTrackId++;
                ghost.position = observer->position + direction * range;
                ghost.uncertainty = range * 0.05;
                ghost.firstSeen = context.now;
                ghost.lastSeen = context.now;
                ghost.ghost = true;
                picture.contacts.push_back(ghost);
            }
        }

        // Loss of contact: tracks not refreshed recently are dropped (ghosts fade faster). A destroyed ship's
        // track ends at once: the explosion is seen by anyone who was tracking it.
        std::erase_if(picture.contacts, [&](const SensorContact& c) {
            return context.now - c.lastSeen > (c.ghost ? kGhostLifetime : kContactTimeout) ||
                   (!c.ghost && !world.isAlive(c.target));
        });
    }
}

void SensorSystem::writeState(BinaryWriter& writer) const {
    writer.io(m_pictures);
}

void SensorSystem::readState(BinaryReader& reader) {
    std::vector<FactionPicture> pictures;
    reader.io(pictures);
    if (reader.ok()) {
        m_pictures = std::move(pictures);
    }
}

} // namespace gx
