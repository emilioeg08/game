#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Text/Localization.h"
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

enum Faction : u32 {
    kFactionPlayer = 0,
    kFactionIndependent = 1,
    kFactionPirates = 2,
    kFactionAuthority = 3,
    kFactionCount
};

inline constexpr std::array<const char*, kFactionCount> kFactionNames = {
    GX_TEXT("Jugador"), GX_TEXT("Transportistas independientes"), GX_TEXT("Piratas"), GX_TEXT("Autoridad")};

enum ShipClass : u32 {
    kShipClassCourier = 0,
    kShipClassHauler = 1,
    kShipClassRaider = 2,
    kShipClassPatrol = 3,
    kShipClassMiner = 4,
    kShipClassEscort = 5,
    kShipClassCount
};

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
    {GX_TEXT("Correo"), 50'000.0, 15'000'000.0, 1.5e9, 5.0, 40.0},    // 1 AU in hyperspace: ~100 s
    {GX_TEXT("Carguero"), 15'000.0, 6'000'000.0, 6.0e8, 12.0, 120.0}, // 1 AU in hyperspace: ~250 s
    {GX_TEXT("Corsario"), 40'000.0, 12'000'000.0, 1.2e9, 6.0, 60.0},  // hunts haulers where they drop out
    {GX_TEXT("Patrullero"), 45'000.0, 14'000'000.0, 1.4e9, 5.0,
     70.0},                                                         // the Authority's: faster than a raider
    {GX_TEXT("Minero"), 12'000.0, 5'000'000.0, 5.5e8, 14.0, 110.0}, // slow and heavy: lasers and a big hold
    {GX_TEXT("Escolta"), 42'000.0, 15'000'000.0, 1.5e9, 5.0, 65.0}, // keeps up with a Correo
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
    {1e3, 1e6, 1e3, 1e12, 6.25e34},    // Correo: small, quiet, good sensors and a radar
    {5e3, 2e6, 1e4, 5e11, 0.0},        // Carguero: bigger and louder, basic passive sensors only
    {2e3, 1.5e6, 3e3, 1e12, 3e34},     // Corsario: quiet while it waits; its radar is for the kill
    {3e3, 1.5e6, 5e3, 2e12, 5e34},     // Patrullero: overt (radar on), the best sensors in the system
    {6e3, 2e6, 1.2e4, 5e11, 0.0},      // Minero: a Carguero's sensors on a bigger, warmer hull (the lasers)
    {2.5e3, 1.5e6, 4e3, 1.5e12, 4e34}, // Escolta: a light warship with a fire-control radar
}};

// Weapons (ADR-025). Beams: damage per second while the aim error stays within the hull. Railguns: damage
// per hit; at 5,000 km/s a slug needs 0.6 s to cross 3,000 km, enough for an accelerating hauler (15 km/s^2)
// to be ~3 km away from where fire control predicted. Damage is tuned so that a duel lasts about a minute at
// x1: long enough to decide to flee, jink or turn the radar off.
enum Weapon : u32 { kWeaponLaser = 0, kWeaponRailgun = 1, kWeaponCount };

inline std::vector<WeaponDef> weaponTable() {
    return {
        {GX_TEXT("Láser"), WeaponKind::Beam, 800'000.0, 4.0, 0.0, 0.0, 1e-5},
        {GX_TEXT("Cañón de riel"), WeaponKind::Projectile, 3'000'000.0, 15.0, 2.0, 5'000'000.0, 2e-5},
    };
}

// Display names of the module types, indexed by ModuleType.
inline constexpr std::array<const char*, static_cast<usize>(ModuleType::Count)> kModuleNames = {
    GX_TEXT("Estructura"), GX_TEXT("Reactor"),    GX_TEXT("Motor"),
    GX_TEXT("Hipermotor"), GX_TEXT("Sensores"),   GX_TEXT("Arma"),
    GX_TEXT("Bodega"),     GX_TEXT("Habitáculo"), GX_TEXT("Láser de minería")};

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

inline constexpr std::array<ModuleDef, 9> kPatrolModules = {{
    {ModuleType::Structure, 600.0},
    {ModuleType::Reactor, 150.0},
    {ModuleType::Drive, 180.0},
    {ModuleType::HyperDrive, 140.0},
    {ModuleType::Sensors, 120.0},
    {ModuleType::Weapon, 90.0, kWeaponLaser},
    {ModuleType::Weapon, 90.0, kWeaponLaser},
    {ModuleType::Weapon, 90.0, kWeaponRailgun},
    {ModuleType::Quarters, 100.0},
}};

inline constexpr std::array<ModuleDef, 9> kMinerModules = {{
    {ModuleType::Structure, 650.0},
    {ModuleType::Reactor, 160.0},
    {ModuleType::Drive, 180.0},
    {ModuleType::HyperDrive, 140.0},
    {ModuleType::Sensors, 80.0},
    {ModuleType::Mining, 150.0},
    {ModuleType::Mining, 150.0},
    {ModuleType::Cargo, 500.0},
    {ModuleType::Quarters, 120.0},
}};

inline constexpr std::array<ModuleDef, 9> kEscortModules = {{
    {ModuleType::Structure, 550.0},
    {ModuleType::Reactor, 150.0},
    {ModuleType::Drive, 170.0},
    {ModuleType::HyperDrive, 130.0},
    {ModuleType::Sensors, 110.0},
    {ModuleType::Weapon, 90.0, kWeaponLaser},
    {ModuleType::Weapon, 90.0, kWeaponLaser},
    {ModuleType::Weapon, 90.0, kWeaponRailgun},
    {ModuleType::Quarters, 100.0},
}};

inline std::span<const ModuleDef> shipModules(u32 shipClass) {
    switch (shipClass) {
    case kShipClassCourier:
        return kCourierModules;
    case kShipClassHauler:
        return kHaulerModules;
    case kShipClassPatrol:
        return kPatrolModules;
    case kShipClassMiner:
        return kMinerModules;
    case kShipClassEscort:
        return kEscortModules;
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
    return {{GX_TEXT("Agua"), 20.0},        {GX_TEXT("Alimentos"), 40.0}, {GX_TEXT("Mineral"), 30.0},
            {GX_TEXT("Combustible"), 50.0}, {GX_TEXT("Metales"), 120.0},  {GX_TEXT("Maquinaria"), 300.0}};
}

inline constexpr std::array<u32, kShipClassCount> kCargoCapacity = {20, 100, 30, 0, 120, 0}; // tonnes
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

// --- Authority patrols (ADR-030) ---------------------------------------------------------------------------
// The treasury buys and keeps patrol ships, by explicit budget rules: commission one when the price plus
// kPatrolReserveHours of upkeep for the enlarged fleet is in the bank; decommission when the money runs out.
inline constexpr i64 kPatrolCommissionCost = 6'000;
inline constexpr i64 kPatrolUpkeepPerMinute = 25; // 1,500 cr/h per patrol
inline constexpr f64 kPatrolReserveHours = 4.0;
inline constexpr f64 kPatrolEngageRange = 1e9;     // m: suspects nearer than this are chased
inline constexpr f64 kPatrolGiveUpRange = 2e9;     // m
inline constexpr f64 kPatrolStandoff = 150e3;      // m
inline constexpr f64 kPatrolBeatWellFactor = 1.05; // they watch the edge of a planet's well, like raiders do
inline constexpr f64 kPatrolRetreatStructure = 0.5;
inline constexpr f64 kPatrolRepairedStructure = 0.95;
inline constexpr f64 kPirateWaryRange = 3e8;             // m: raiders break off when a patrol is this close
inline constexpr f64 kDistressLifetime = 180.0;          // s: a call for help is answered for this long
inline constexpr f64 kDistressMerge = 1e8;               // m: calls this close together are the same incident
inline constexpr f64 kReputationAttackAuthority = -15.0; // identified attacking a patrol
inline constexpr f64 kReputationKillAuthority = -40.0;
inline constexpr f64 kReputationAfterDeath = -20.0; // a destroyed hostile player has paid: no longer wanted

// --- Contracts (ADR-031)
// ------------------------------------------------------------------------------------
inline constexpr f64 kContractShortageRatio = 0.25; // stock / target below which a port asks for supplies
inline constexpr u32 kContractTonnes = 20;          // what a courier can carry
inline constexpr f64 kContractPremium = 2.0;        // reward: tonnes x base price x premium
inline constexpr SimDuration kContractDeliveryTime = SimDuration::minutes(45);
inline constexpr i64 kContractBountyReward = 2'500; // on top of the standard bounty
inline constexpr SimDuration kContractBountyTime = SimDuration::minutes(60);
inline constexpr SimDuration kContractHistory = SimDuration::minutes(30); // closed contracts stay listed
inline constexpr usize kMaxOpenContracts = 6;
inline constexpr usize kMaxAcceptedContracts = 3;
inline constexpr i64 kContractTreasuryReserve = 5'000; // plus the patrol budget: patrols come first
inline constexpr f64 kReputationContractDone = 3.0;
inline constexpr f64 kReputationContractFailed = -5.0;

// --- Finance (ADR-033) -------------------------------------------------------------------------------------
// Ships are bought, not spawned. A trader's hull is paid for by its owner's savings and a loan from the
// system's bank, which lends its capital and the deposits of the traders who did well. The traders' mutual
// insures every hull at a premium priced from its own loss experience. Rates are per hour of game time:
// at the real-time scale a trade turns its capital over in minutes (ADR-021).
inline constexpr i64 kHaulerHullPrice = 6'000; // a new Carguero from the yards (outside the system)
inline constexpr f64 kHullRecovery = 0.5;      // share of the hull price a repossessed ship sells for
inline constexpr i64 kBankCapital = 40'000;
inline constexpr f64 kLoanRatePerHour = 0.02;
inline constexpr f64 kDepositRatePerHour = 0.005;   // at most: the bank passes on part of what its loans earn
inline constexpr f64 kDepositPassThrough = 0.5;     // share of the loan interest the deposits can get
inline constexpr f64 kLoanTermHours = 12.0;         // equal principal instalments, paid every minute
inline constexpr f64 kBankReserveRatio = 0.2;       // share of the deposits the bank keeps in cash
inline constexpr f64 kRequiredCoverage = 1.25;      // DSCR: (expected earnings - premium) / debt service
inline constexpr f64 kEarningsPrior = 900.0;        // cr per ship-hour the bank assumes before any evidence
inline constexpr f64 kEarningsPriorHours = 20.0;    // how many ship-hours that assumption weighs
inline constexpr f64 kExperienceHalfLife = 7'200.0; // s: the bank and the mutual forget old experience
inline constexpr i64 kTraderCashReserve = 5'000;    // traders keep this on board; the rest pays debt or saves
inline constexpr i64 kWorkingCapital =
    6'000;                                   // the bank tops a trader's credits up to this (hull as security)
inline constexpr f64 kMinDownPayment = 0.25; // of the hull: the bank lends the rest at most (its LTV)
inline constexpr i64 kNewcomerSavings = 4'500; // an outsider's down payment and first credits to trade
inline constexpr f64 kInitialDebtShare = 0.5;  // at the start, each hull still owes up to this share
inline constexpr SimDuration kBuyerPatience = SimDuration::minutes(30); // then an owner without credit leaves
inline constexpr SimDuration kNewcomerInterval = SimDuration::minutes(5); // outsiders arrive one at a time
inline constexpr i64 kMutualCapital = 40'000;
inline constexpr f64 kPremiumLoading = 0.25; // over the expected claims, while the fund is below its target
inline constexpr i64 kMutualTargetFund = 80'000; // above it the mutual charges only the expected claims
inline constexpr f64 kLossRatePrior = 0.07;      // hull losses per ship-hour before any experience
inline constexpr f64 kClaimCostPrior = 550.0;    // cr of claims per ship-hour: those losses and repairs
inline constexpr f64 kLossRatePriorHours = 40.0; // how many ship-hours of experience the prior weighs
inline constexpr f64 kPremiumNewsChange = 0.25;  // a premium move this large makes the news

// --- Mining (ADR-037)
// ----------------------------------------------------------------------------------------- Asteroid fields
// hold deposits: rock gives ore, ice gives water. A deposit refills at a constant rate up to its size (new
// rocks drift into reach), so a field sustains its recovery rate and no more: mine it faster and it runs dry
// until it recovers.
inline constexpr f64 kFieldDeposit = 4'000.0;     // t
inline constexpr f64 kRockFieldRecovery = 120.0;  // t/h
inline constexpr f64 kIceFieldRecovery = 150.0;   // t/h
inline constexpr f64 kMiningRatePerModule = 60.0; // t/h per mining laser at full health
inline constexpr f64 kMiningRange = 60'000.0;     // m from the field's centre (its extent)
inline constexpr f64 kMiningSpeed = 200.0;        // m/s relative to the field: faster, the lasers cannot hold

inline constexpr Good fieldGood(BodyKind kind) {
    return kind == BodyKind::IceField ? kGoodWater : kGoodOre;
}

// --- The player's company (ADR-037)
// --------------------------------------------------------------------------- Ships the player buys at a
// station's yards, crewed by hired captains who follow standing orders. One account pays for everything; the
// bank lends against the fleet's hulls and the mutual insures them.
inline constexpr std::array<i64, kShipClassCount> kShipPrices = {4'000, 6'000, 0, 0, 5'500, 7'000};
inline constexpr std::array<i64, kShipClassCount> kShipWagesPerMinute = {0, 5, 0, 0, 6, 8}; // crews
inline constexpr std::array<u32, 3> kShipsForSale = {kShipClassHauler, kShipClassMiner, kShipClassEscort};
inline constexpr usize kMaxFleet = 12;          // ships besides the one the player flies
inline constexpr f64 kShipResale = 0.7;         // of the hull value, selling to the yards
inline constexpr i64 kFleetCashReserve = 1'000; // automatic trade never spends the account below this
// Insured hauler-hours at which the company's own record weighs as much as the mutual's. Experience fades
// (kExperienceHalfLife), so one ship's record never exceeds ~2.9 hauler-hours: it earns a third of the
// weight, a fleet of five, two thirds.
inline constexpr f64 kCredibilityHours = 5.0;
// Overdrawn beyond what the cargo on board will fetch for this long, a ship is sold (illiquid is not
// insolvent: a miner on its way to market with a full hold covers the bills it will pay).
inline constexpr SimDuration kArrearsGrace = SimDuration::minutes(30);
inline constexpr f64 kEscortStandoff = 3'000.0; // m from the flagship
inline constexpr f64 kEscortEngageRange = 3e8;  // m from the flagship: raiders nearer than this are engaged
inline constexpr f64 kEscortLeash = 6e8;        // m: escorts break off a chase this far from the flagship
inline constexpr f64 kFleetRetreatStructure = 0.4; // escorts go for repairs below this
inline constexpr f64 kFleetRepairedStructure = 0.9;

// --- Wrecks and salvage (ADR-039)
// ---------------------------------------------------------------------------
inline constexpr SimDuration kWreckLifetime = SimDuration::hours(3); // then it drifts out of reach
inline constexpr f64 kWreckSightRange = 5e7;                         // m: a ship this close finds a wreck
inline constexpr f64 kSalvageRange = 5'000.0;                        // m
inline constexpr f64 kSalvageSpeed = 200.0;                          // m/s, relative

inline constexpr std::array<const char*, 12> kFleetNames = {"Aurora",  "Boreal",  "Cénit",   "Delfín",
                                                            "Eclipse", "Fortuna", "Gaviota", "Horizonte",
                                                            "Iris",    "Júpiter", "Kraken",  "Lucero"};

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

inline constexpr std::array<const char*, 6> kPatrolNames = {"Custodia", "Égida",       "Centinela",
                                                            "Baluarte", "Salvaguarda", "Templanza"};

} // namespace gx::content
