#pragma once

#include "Engine/Core/Types.h"
#include "Simulation/Economy/Economy.h"
#include "Space/Bodies/CelestialBody.h"
#include "Space/Combat/Combat.h"
#include "Space/Ships/Modules.h"

#include <array>
#include <span>
#include <vector>

// Provisional content (docs/DESIGN.md). Kept as plain tables behind one header so it can move to data files
// (the modding boundary) without touching the systems that use it.
namespace gx::content {

// Calendar year at SimTime::epoch() for display.
inline constexpr i64 kEpochYear = 2400;

enum Faction : u32 { kFactionPlayer = 0, kFactionIndependent = 1, kFactionPirates = 2, kFactionCount };

inline constexpr std::array<const char*, kFactionCount> kFactionNames = {
    "Jugador", "Transportistas independientes", "Piratas"};

enum ShipClass : u32 { kShipClassCourier = 0, kShipClassHauler = 1, kShipClassRaider = 2, kShipClassCount };

struct ShipClassDef {
    const char* name;
    f64 maxAcceleration;      // m/s^2, sublight
    f64 cruiseSpeed;          // m/s, sublight autopilot limit
    f64 hyperspaceSpeed;      // m/s
    f64 hyperspaceChargeTime; // s
    f64 hullRadius;           // m, for hit tests
};

// Real-time scale (x1 = real time, ADR-021): a trip between planets takes minutes at x1, seconds at x10.
inline constexpr std::array<ShipClassDef, kShipClassCount> kShipClasses = {{
    {"Correo", 50'000.0, 15'000'000.0, 1.5e9, 5.0, 40.0},    // 1 AU in hyperspace: ~100 s
    {"Carguero", 15'000.0, 6'000'000.0, 6.0e8, 12.0, 120.0}, // 1 AU in hyperspace: ~250 s
    {"Corsario", 40'000.0, 12'000'000.0, 1.2e9, 6.0, 60.0},  // hunts haulers where they drop out
}};

// Sensors and signatures (ADR-023). Emission units are arbitrary; ranges follow from the SNR formulas:
// an idle Correo is seen passively at ~22,000 km by a Carguero, at ~700,000 km while thrusting hard; any ship
// in hyperspace is visible across most of the system; the Correo's radar identifies a Carguero at ~3 million
// km.
struct SensorDef {
    f64 baseEmission;
    f64 driveEmission;
    f64 crossSection; // m^2
    f64 passiveSensitivity;
    f64 activeStrength; // 0: no radar
};

inline constexpr std::array<SensorDef, kShipClassCount> kShipSensors = {{
    {1e3, 1e6, 1e3, 1e12, 6.25e34}, // Correo: small, quiet, good sensors and a radar
    {5e3, 2e6, 1e4, 5e11, 0.0},     // Carguero: bigger and louder, basic passive sensors only
    {2e3, 1.5e6, 3e3, 1e12, 3e34},  // Corsario: quiet while it waits; its radar is for the kill
}};

// Weapons (ADR-025). Beams: damage per second while the aim error stays within the hull. Railguns: damage
// per hit; at 5,000 km/s a slug needs 0.6 s to cross 3,000 km, enough for an accelerating hauler (15 km/s^2)
// to be ~3 km away from where fire control predicted. Damage is tuned so that a duel lasts about a minute at
// x1: long enough to decide to flee, jink or turn the radar off.
enum Weapon : u32 { kWeaponLaser = 0, kWeaponRailgun = 1, kWeaponCount };

inline std::vector<WeaponDef> weaponTable() {
    return {
        {"Láser", WeaponKind::Beam, 800'000.0, 4.0, 0.0, 0.0, 1e-5},
        {"Cañón de riel", WeaponKind::Projectile, 3'000'000.0, 15.0, 2.0, 5'000'000.0, 2e-5},
    };
}

// Display names of the module types, indexed by ModuleType.
inline constexpr std::array<const char*, static_cast<usize>(ModuleType::Count)> kModuleNames = {
    "Estructura", "Reactor", "Motor", "Hipermotor", "Sensores", "Arma", "Bodega", "Habitáculo"};

struct ModuleDef {
    ModuleType type;
    f64 health;
    u32 weapon = 0;
};

// Ship designs: the structure first, then the modules (a hit lands on a module in proportion to its size).
inline constexpr std::array<ModuleDef, 8> kCourierModules = {{
    {ModuleType::Structure, 500.0},
    {ModuleType::Reactor, 150.0},
    {ModuleType::Drive, 150.0},
    {ModuleType::HyperDrive, 120.0},
    {ModuleType::Sensors, 100.0},
    {ModuleType::Weapon, 80.0, kWeaponLaser},
    {ModuleType::Weapon, 80.0, kWeaponRailgun},
    {ModuleType::Quarters, 80.0},
}};

inline constexpr std::array<ModuleDef, 7> kHaulerModules = {{
    {ModuleType::Structure, 600.0},
    {ModuleType::Reactor, 150.0},
    {ModuleType::Drive, 200.0},
    {ModuleType::HyperDrive, 150.0},
    {ModuleType::Sensors, 80.0},
    {ModuleType::Cargo, 600.0},
    {ModuleType::Quarters, 150.0},
}};

inline constexpr std::array<ModuleDef, 9> kRaiderModules = {{
    {ModuleType::Structure, 400.0},
    {ModuleType::Reactor, 120.0},
    {ModuleType::Drive, 150.0},
    {ModuleType::HyperDrive, 120.0},
    {ModuleType::Sensors, 80.0},
    {ModuleType::Weapon, 80.0, kWeaponLaser},
    {ModuleType::Weapon, 80.0, kWeaponLaser},
    {ModuleType::Weapon, 80.0, kWeaponRailgun},
    {ModuleType::Quarters, 80.0},
}};

inline std::span<const ModuleDef> shipModules(u32 shipClass) {
    switch (shipClass) {
    case kShipClassCourier:
        return kCourierModules;
    case kShipClassHauler:
        return kHaulerModules;
    default:
        return kRaiderModules;
    }
}

// Behaviour tuning (game rules, not physics).
inline constexpr f64 kDockRepairRate = 0.02;          // share of max health per second, docked at a station
inline constexpr f64 kDamageControlRate = 0.0025;     // reactor and drive, anywhere, out of combat
inline constexpr f64 kDamageControlCap = 0.25;        // damage control cannot do better than this
inline constexpr f64 kDamageControlDelay = 10.0;      // s without being hit before damage control starts
inline constexpr f64 kPlayerPursuitStandoff = 300e3;  // m: well inside laser range
inline constexpr f64 kPirateStandoff = 200e3;         // m
inline constexpr f64 kPirateHuntRange = 3e8;          // m: prey farther than this is ignored
inline constexpr f64 kPirateGiveUpRange = 6e8;        // m: a hunt is abandoned beyond this
inline constexpr f64 kPirateFleeStructure = 0.4;      // structure share below which a raider leaves
inline constexpr f64 kStationSafeRadius = 50e6;       // m: raiders do not attack near a station's guns
inline constexpr f64 kHyperspaceSpeedThreshold = 1e8; // m/s: tracks faster than this are in hyperspace

// --- Economy (ADR-027) ------------------------------------------------------------------------------------
// A supply chain in one system: planets extract water, food, ore and fuel; the refinery turns ore and fuel
// into metals, the factory metals and fuel into machinery, and machinery keeps the planets' extraction
// running (upkeep). Populations everywhere eat and drink. Rates are tonnes per hour at x1 (real time).

enum Good : u32 { kGoodWater, kGoodFood, kGoodOre, kGoodFuel, kGoodMetals, kGoodMachinery, kGoodCount };

inline std::vector<GoodDef> goodTable() {
    return {{"Agua", 20.0},        {"Alimentos", 40.0}, {"Mineral", 30.0},
            {"Combustible", 50.0}, {"Metales", 120.0},  {"Maquinaria", 300.0}};
}

inline constexpr std::array<u32, kShipClassCount> kCargoCapacity = {20, 100, 30}; // tonnes
inline constexpr i64 kPlayerStartCredits = 2'000;
inline constexpr i64 kHaulerStartCredits = 3'000;
inline constexpr f64 kMarketStockHours = 4.0; // target stock: four hours of the larger of output and use
inline constexpr f64 kMarketCapacityFactor = 3.0;
inline constexpr f64 kMarketMinTarget = 100.0; // tonnes
inline constexpr f64 kSupplyMargin = 1.2;      // total output of every good over its total use, at generation

// Hauler trading rules.
inline constexpr i64 kHaulerMinProfit = 150;       // credits: below this a trip is not worth loading
inline constexpr f64 kHaulerMaxMarketShare = 0.5;  // never empty more than half of a market's stock
inline constexpr f64 kKnowledgeHalfLife = 1'800.0; // s: a price seen 30 min ago counts half
inline constexpr f64 kDangerHalfLife = 1'200.0;    // s: memory of a loss near a port
inline constexpr f64 kTripOverhead = 180.0;        // s: charging, wells and docking in a trip estimate
inline constexpr f64 kExploreAfter = 1'200.0;      // s: prices older than this are worth refreshing

// --- Consequences (ADR-029) -------------------------------------------------------------------------------
// The system's Authority taxes trade, charges for station repairs and pays bounties on pirates. Crews cost
// wages. The traders' network reports whoever it catches attacking its ships: reputation.
inline constexpr i64 kStartingTreasury = 20'000;
inline constexpr f64 kTradeTaxRate = 0.03;        // of each tonne's price, buying and selling
inline constexpr i64 kHaulerWagesPerMinute = 5;   // 300 cr/h per hauler crew
inline constexpr f64 kRepairCostPerPoint = 3.0;   // credits per point of module health, at a station
inline constexpr i64 kPirateBounty = 1'500;       // destroying or capturing a raider
inline constexpr f64 kReputationHit = -10.0;      // identified attacking an independent ship
inline constexpr f64 kReputationKill = -30.0;     // identified destroying one
inline constexpr f64 kReputationBoarding = -20.0; // boarding one (they always know who boarded them)
inline constexpr f64 kReputationPirateKill = 5.0;
inline constexpr f64 kReputationRecoveryPerMinute = 0.5; // bad reputation fades back towards neutral
inline constexpr f64 kHostileReputation = -30.0; // at or below: ports refuse trade, repairs, bulletins
inline constexpr f64 kMaxReputation = 100.0;
inline constexpr f64 kBoardingRange = 5'000.0; // m
inline constexpr f64 kBoardingSpeed = 200.0;   // m/s, relative
inline constexpr f64 kOffenseMemory = 60.0;    // s: repeated hits on one ship count as one offence

enum class PortRole : u8 { Planet, Refinery, Factory, Industry };

// Economic profile of a port. Targets and capacities are filled in by the caller from the rates.
inline Market portMarket(BodyKind kind, PortRole role) {
    const RecipeInput upkeep2{kGoodMachinery, 0.02, false};
    const RecipeInput upkeep3{kGoodMachinery, 0.03, false};
    Market market;
    const auto produce = [&](Good output, f64 rate, std::vector<RecipeInput> inputs) {
        market.recipes.push_back({output, rate, std::move(inputs)});
    };
    const auto consume = [&](Good good, f64 rate) { market.demands.push_back({good, rate}); };
    switch (kind) {
    case BodyKind::IcePlanet:
        produce(kGoodWater, 240.0, {upkeep2});
        consume(kGoodFood, 40.0);
        consume(kGoodFuel, 10.0);
        break;
    case BodyKind::OceanPlanet:
        produce(kGoodFood, 240.0, {upkeep2});
        consume(kGoodFuel, 20.0);
        break;
    case BodyKind::RockyPlanet:
        produce(kGoodOre, 240.0, {upkeep3});
        consume(kGoodFood, 40.0);
        consume(kGoodWater, 40.0);
        break;
    case BodyKind::DesertPlanet:
        produce(kGoodOre, 180.0, {upkeep3});
        consume(kGoodFood, 40.0);
        consume(kGoodWater, 60.0);
        break;
    case BodyKind::GasGiant:
        produce(kGoodFuel, 240.0, {upkeep3});
        consume(kGoodFood, 30.0);
        consume(kGoodWater, 30.0);
        break;
    case BodyKind::Station:
        if (role == PortRole::Refinery || role == PortRole::Industry) {
            produce(kGoodMetals, 120.0, {{kGoodOre, 2.0, true}, {kGoodFuel, 0.5, true}});
        }
        if (role == PortRole::Factory || role == PortRole::Industry) {
            produce(kGoodMachinery, 40.0, {{kGoodMetals, 2.0, true}, {kGoodFuel, 0.5, true}});
        }
        consume(kGoodFood, 50.0);
        consume(kGoodWater, 50.0);
        break;
    default:
        break;
    }
    return market;
}

inline constexpr const char* kPlayerShipName = "Errante";

inline constexpr std::array<const char*, 16> kHaulerNames = {
    "Brisa", "Faro",     "Halcón", "Lince",  "Marea", "Nómada", "Ceniza",   "Alba",
    "Sirga", "Torrente", "Vigía",  "Quilla", "Ámbar", "Estela", "Albatros", "Cierzo"};

inline constexpr std::array<const char*, 8> kRaiderNames = {"Colmillo", "Garfio", "Sombra",  "Espina",
                                                            "Tábano",   "Cuervo", "Carroña", "Zarpa"};

} // namespace gx::content
