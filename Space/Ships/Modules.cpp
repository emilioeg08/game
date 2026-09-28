#include "Space/Ships/Modules.h"

#include "Simulation/World/World.h"
#include "Space/Sensors/Sensors.h"
#include "Space/Ships/Ship.h"

#include <algorithm>

namespace gx {

const char* toString(ModuleType type) {
    switch (type) {
    case ModuleType::Structure:
        return "Structure";
    case ModuleType::Reactor:
        return "Reactor";
    case ModuleType::Drive:
        return "Drive";
    case ModuleType::HyperDrive:
        return "HyperDrive";
    case ModuleType::Sensors:
        return "Sensors";
    case ModuleType::Weapon:
        return "Weapon";
    case ModuleType::Cargo:
        return "Cargo";
    case ModuleType::Quarters:
        return "Quarters";
    case ModuleType::Mining:
        return "Mining";
    case ModuleType::Count:
        break;
    }
    return "Unknown";
}

f64 moduleHealth(const ShipModules& modules, ModuleType type) {
    f64 total = 0.0;
    u32 count = 0;
    for (const ShipModule& module : modules.modules) {
        if (module.type == type) {
            total += module.fraction();
            ++count;
        }
    }
    return count == 0 ? 0.0 : total / count;
}

f64 moduleEfficiency(const ShipModules& modules, ModuleType type) {
    f64 total = 0.0;
    u32 count = 0;
    for (const ShipModule& module : modules.modules) {
        if (module.type == type) {
            total += module.functional() ? module.fraction() : 0.0;
            ++count;
        }
    }
    return count == 0 ? 0.0 : total / count;
}

bool hasPower(const ShipModules& modules) {
    return moduleEfficiency(modules, ModuleType::Reactor) > 0.0;
}

const ShipModule* structureOf(const ShipModules& modules) {
    for (const ShipModule& module : modules.modules) {
        if (module.type == ModuleType::Structure) {
            return &module;
        }
    }
    return nullptr;
}

DamageResult applyDamage(ShipModules& modules, f64 damage, Rng& rng) {
    DamageResult result;
    ShipModule* structure = nullptr;
    f64 totalSize = 0.0;
    for (ShipModule& module : modules.modules) {
        if (module.type == ModuleType::Structure) {
            structure = &module;
        } else {
            totalSize += module.maxHealth;
        }
    }
    if (structure == nullptr) {
        return result;
    }

    f64 structureDamage = damage * kStructureDamageShare;
    // Pick the module in proportion to its size (a single draw: deterministic for a given stream).
    ShipModule* hit = nullptr;
    if (totalSize > 0.0) {
        f64 pick = rng.uniform(0.0, totalSize);
        for (ShipModule& module : modules.modules) {
            if (module.type == ModuleType::Structure) {
                continue;
            }
            hit = &module;
            pick -= module.maxHealth;
            if (pick < 0.0) {
                break;
            }
        }
    }
    if (hit != nullptr && hit->health > 0.0) {
        const bool wasFunctional = hit->functional();
        hit->health = std::max(0.0, hit->health - damage);
        result.moduleHit = hit->type;
        result.moduleDestroyed = wasFunctional && !hit->functional();
        if (result.moduleDestroyed && hit->type == ModuleType::Reactor && rng.chance(kReactorBreachChance)) {
            result.reactorBreach = true;
        }
    } else {
        structureDamage += damage; // nothing left there to absorb it
    }
    structure->health = std::max(0.0, structure->health - structureDamage);
    result.shipDestroyed = result.reactorBreach || structure->health <= 0.0;
    return result;
}

void applyModuleEffects(World& world, EntityId ship) {
    const ShipModules* modules = world.components<ShipModules>().tryGet(ship);
    const ShipDesignStats* design = world.components<ShipDesignStats>().tryGet(ship);
    if (modules == nullptr || design == nullptr) {
        return;
    }
    const f64 power = hasPower(*modules) ? 1.0 : 0.0;
    if (ShipDrive* drive = world.components<ShipDrive>().tryGet(ship)) {
        const f64 thrust = moduleEfficiency(*modules, ModuleType::Drive) * power;
        drive->maxAcceleration = design->maxAcceleration * thrust;
        drive->cruiseSpeed = design->cruiseSpeed * std::max(thrust, 0.1); // limp home rather than stall
        const f64 hyper = moduleEfficiency(*modules, ModuleType::HyperDrive) * power;
        drive->hyperspaceSpeed = hyper >= 0.5 ? design->hyperspaceSpeed : 0.0;
        drive->hyperspaceChargeTime = design->hyperspaceChargeTime / std::max(hyper, 0.25);
    }
    if (SensorSuite* sensors = world.components<SensorSuite>().tryGet(ship)) {
        const f64 sensing = moduleEfficiency(*modules, ModuleType::Sensors);
        sensors->passiveSensitivity = design->passiveSensitivity * std::max(sensing, 0.05);
        sensors->activeStrength = design->activeStrength * sensing * power;
        if (sensors->activeStrength <= 0.0) {
            sensors->activeOn = false;
        }
    }
}

} // namespace gx
