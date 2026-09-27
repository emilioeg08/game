#pragma once

#include "Engine/Core/Random.h"
#include "Engine/Core/Types.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/World/EntityRegistry.h"

#include <vector>

// Localized damage (prompt §12, ADR-024): a ship is a set of modules with their own health, not a single hit
// point pool. Performance follows module health, and derived stats (acceleration, hyperspace, sensors) are
// recomputed only when modules change, never every tick.
namespace gx {

class World;

enum class ModuleType : u8 {
    Structure, // hull integrity: the ship is destroyed when it reaches zero
    Reactor,   // power: without it drives, weapons and radar are dead (the ship is disabled)
    Drive,     // sublight thrust
    HyperDrive,
    Sensors,
    Weapon,
    Cargo,
    Quarters,
    Count
};

[[nodiscard]] const char* toString(ModuleType type);

// Below this share of its health a module is out of action (it still absorbs hits and can be repaired).
inline constexpr f64 kModuleFunctionalFraction = 0.1;
// Chance that a reactor destroyed by a hit breaches and takes the whole ship with it.
inline constexpr f64 kReactorBreachChance = 0.3;
// Share of every hit that the structure absorbs on top of the module hit.
inline constexpr f64 kStructureDamageShare = 0.5;

struct ShipModule {
    ModuleType type = ModuleType::Structure;
    u32 weapon = 0; // index into the weapon table, for Weapon modules
    f64 maxHealth = 0.0;
    f64 health = 0.0;
    f64 cooldown = 0.0; // s until a Weapon module can fire again

    [[nodiscard]] f64 fraction() const { return maxHealth > 0.0 ? health / maxHealth : 0.0; }
    [[nodiscard]] bool functional() const { return fraction() >= kModuleFunctionalFraction; }

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("type", type);
        ar.io("weapon", weapon);
        ar.io("maxHealth", maxHealth);
        ar.io("health", health);
        ar.io("cooldown", cooldown);
    }
};

struct ShipModules {
    std::vector<ShipModule> modules; // modules[0] is the structure
    SimTime lastDamaged;             // damage control waits for a lull in the fighting

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("modules", modules);
        ar.io("lastDamaged", lastDamaged);
    }
};

// Undamaged performance of the design; the live ShipDrive and SensorSuite are derived from it.
struct ShipDesignStats {
    f64 maxAcceleration = 0.0;
    f64 cruiseSpeed = 0.0;
    f64 hyperspaceSpeed = 0.0;
    f64 hyperspaceChargeTime = 0.0;
    f64 passiveSensitivity = 0.0;
    f64 activeStrength = 0.0;
    f64 hullRadius = 0.0; // m, for hit tests

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("maxAcceleration", maxAcceleration);
        ar.io("cruiseSpeed", cruiseSpeed);
        ar.io("hyperspaceSpeed", hyperspaceSpeed);
        ar.io("hyperspaceChargeTime", hyperspaceChargeTime);
        ar.io("passiveSensitivity", passiveSensitivity);
        ar.io("activeStrength", activeStrength);
        ar.io("hullRadius", hullRadius);
    }
};

// Average health share of the modules of a type (0 if the ship has none). For display.
[[nodiscard]] f64 moduleHealth(const ShipModules& modules, ModuleType type);
// Like moduleHealth, but modules out of action count as zero. Drives performance.
[[nodiscard]] f64 moduleEfficiency(const ShipModules& modules, ModuleType type);
[[nodiscard]] bool hasPower(const ShipModules& modules);
[[nodiscard]] const ShipModule* structureOf(const ShipModules& modules);

struct DamageResult {
    ModuleType moduleHit = ModuleType::Structure;
    bool moduleDestroyed = false; // the hit put the module out of action
    bool reactorBreach = false;
    bool shipDestroyed = false;
};

// Applies one hit: the structure absorbs kStructureDamageShare of it and one module, chosen at random in
// proportion to its size (maxHealth), takes all of it (a module already wrecked passes the hit on to the
// structure). A reactor put out of action may breach and destroy the ship.
DamageResult applyDamage(ShipModules& modules, f64 damage, Rng& rng);

// Recomputes ShipDrive and SensorSuite from ShipDesignStats and module efficiency:
//   no reactor      -> no thrust, no hyperspace, no radar (disabled, drifting)
//   drive           -> acceleration scales with drive efficiency
//   hyperdrive      -> unusable below 50%; charging takes longer as it degrades
//   sensors         -> sensitivity and radar strength scale with sensor efficiency
void applyModuleEffects(World& world, EntityId ship);

} // namespace gx
