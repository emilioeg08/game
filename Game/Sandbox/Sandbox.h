#pragma once

#include "Engine/Core/Random.h"
#include "Engine/Core/Types.h"
#include "Engine/Math/Vec3.h"
#include "Engine/Text/Localization.h"
#include "Engine/Time/SimTime.h"
#include "Game/Sandbox/Contracts.h"
#include "Game/Sandbox/Fleet.h"
#include "Simulation/Economy/Economy.h"
#include "Simulation/Economy/Finance.h"
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
    // The Authority keeps at most this many patrols, as far as its treasury allows (ADR-030).
    u32 maxPatrols = 3;
    SimDuration authorityReview = SimDuration::minutes(2);
    // Traders take supply contracts too, competing with the player (ADR-032). Off: only the player does.
    bool tradersTakeContracts = true;
    // Ships are bought with savings and bank credit, and insured by the traders' mutual (ADR-033). Off: lost
    // or bankrupt traders are replaced for free, with fresh capital, up to `haulers` (the M3.4 behaviour).
    bool finance = true;
    // Most traders the system's business can grow to (0: twice `haulers`). Only with `finance`.
    u32 maxHaulers = 0;

    [[nodiscard]] u32 fleetCap() const {
        return finance ? (maxHaulers > 0 ? maxHaulers : 2 * haulers) : haulers;
    }
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

// Player input: board a disabled ship next to yours (a sensor track) and take its cargo.
struct BoardCommand {
    EntityId ship;
    u32 track = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("ship", ship);
        ar.io("track", track);
    }
};

// NPC hauler behaviour state: wait at a port, then fly to another one.
struct HaulerBrain {
    SimTime departAt;
    EntityId lastPort;
    u32 trips = 0;
    bool retiring = false; // bankrupt: leaves the system instead of departing

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("departAt", departAt);
        ar.io("lastPort", lastPort);
        ar.io("trips", trips);
        ar.io("retiring", retiring);
    }
};

// A trader's books (ADR-033): what it owes the bank, what it saves there and what its hull is insured for.
// Earnings are measured against `lastWorth`, its net worth after the previous finance review.
struct TraderFinance {
    i64 debt = 0;
    i64 instalment = 0; // principal due every minute
    i64 deposit = 0;
    i64 hullValue = 0;
    i64 lastWorth = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("debt", debt);
        ar.io("instalment", instalment);
        ar.io("deposit", deposit);
        ar.io("hullValue", hullValue);
        ar.io("lastWorth", lastWorth);
    }
};

// An owner-operator who lost its ship (destroyed, with an insurance payout) and wants another. Its equity
// waits as a deposit at the bank until the bank finances the rest of a hull, or its patience runs out.
struct ShipBuyer {
    std::string name;
    i64 equity = 0;
    SimTime readyAt;
    SimTime giveUpAt;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("name", name);
        ar.io("equity", equity);
        ar.io("readyAt", readyAt);
        ar.io("giveUpAt", giveUpAt);
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

enum class PatrolState : u8 { Patrolling, Engaging, Returning, Count };

[[nodiscard]] const char* toString(PatrolState state);

// Authority patrol behaviour state. Decisions use only the Authority's sensor picture.
struct PatrolBrain {
    PatrolState state = PatrolState::Patrolling;
    SimTime nextMove;    // Patrolling: when to move to another beat; Engaging: when to give up
    u32 decisions = 0;   // key for the per-decision random streams
    u32 ignoreTrack = 0; // a suspect given up on

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("state", state);
        ar.io("nextMove", nextMove);
        ar.io("decisions", decisions);
        ar.io("ignoreTrack", ignoreTrack);
    }
};

// Player: what happens to you. News: what the Authority or the markets announce. Traffic: NPC comings and
// goings (plenty of them: the client hides them unless asked). Fleet: the routine of the company's ships.
enum class JournalKind : u8 { Player, News, Traffic, Fleet, Count };

// What the journal records is a Message, rendered in the player's language when shown (ADR-036).
struct JournalEntry {
    SimTime time;
    Message text;
    JournalKind kind = JournalKind::Player;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("time", time);
        ar.io("text", text);
        ar.io("kind", kind);
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
    i64 taxesCollected = 0;
    i64 wagesPaid = 0;
    i64 repairFees = 0;
    i64 bountiesPaid = 0;
    u64 bankruptcies = 0;
    u64 boardings = 0;
    u64 patrolsCommissioned = 0;
    u64 patrolsDecommissioned = 0;
    u64 patrolsLost = 0;
    i64 patrolUpkeepPaid = 0;
    u64 piratesKilledByPatrols = 0;
    u64 wantedChases = 0; // patrols going after a hostile player
    u64 distressCalls = 0;
    u64 distressAnswered = 0;
    u64 contractsPosted = 0;
    u64 contractsCompleted = 0;
    u64 contractsFailed = 0;
    u64 contractsExpired = 0;
    u64 contractsCancelled = 0;
    i64 contractRewardsPaid = 0;
    u64 contractsCompletedByTraders = 0;
    // Finance (ADR-033).
    u64 shipsBought = 0;
    u64 shipsByReturningOwners = 0; // rebuilt with an insurance payout
    u64 shipsByExpansion = 0;       // bought by a trader out of its savings
    u64 shipsByOutsiders = 0;       // newcomers with outside savings and a loan
    u64 repossessions = 0;
    u64 ownersRetired = 0; // left the system with what was left of their equity
    u64 peakHaulers = 0;
    i64 capitalIn = 0;  // outsiders' savings brought into the system
    i64 capitalOut = 0; // equity taken out by owners who left
    i64 shipyardPaid = 0;
    i64 premiumsPaid = 0;

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
        ar.io("taxesCollected", taxesCollected);
        ar.io("wagesPaid", wagesPaid);
        ar.io("repairFees", repairFees);
        ar.io("bountiesPaid", bountiesPaid);
        ar.io("bankruptcies", bankruptcies);
        ar.io("boardings", boardings);
        ar.io("patrolsCommissioned", patrolsCommissioned);
        ar.io("patrolsDecommissioned", patrolsDecommissioned);
        ar.io("patrolsLost", patrolsLost);
        ar.io("patrolUpkeepPaid", patrolUpkeepPaid);
        ar.io("piratesKilledByPatrols", piratesKilledByPatrols);
        ar.io("wantedChases", wantedChases);
        ar.io("distressCalls", distressCalls);
        ar.io("distressAnswered", distressAnswered);
        ar.io("contractsPosted", contractsPosted);
        ar.io("contractsCompleted", contractsCompleted);
        ar.io("contractsFailed", contractsFailed);
        ar.io("contractsExpired", contractsExpired);
        ar.io("contractsCancelled", contractsCancelled);
        ar.io("contractRewardsPaid", contractRewardsPaid);
        ar.io("contractsCompletedByTraders", contractsCompletedByTraders);
        ar.io("shipsBought", shipsBought);
        ar.io("shipsByReturningOwners", shipsByReturningOwners);
        ar.io("shipsByExpansion", shipsByExpansion);
        ar.io("shipsByOutsiders", shipsByOutsiders);
        ar.io("repossessions", repossessions);
        ar.io("ownersRetired", ownersRetired);
        ar.io("peakHaulers", peakHaulers);
        ar.io("capitalIn", capitalIn);
        ar.io("capitalOut", capitalOut);
        ar.io("shipyardPaid", shipyardPaid);
        ar.io("premiumsPaid", premiumsPaid);
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

// A trader under fire calls for help through the network: where, and when (patrols answer it).
struct DistressCall {
    Vec3d position;
    SimTime time;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("position", position);
        ar.io("time", time);
    }
};

// A recent attack by the player on an independent ship (repeated hits count once).
struct Offense {
    EntityId victim;
    SimTime time;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("victim", victim);
        ar.io("time", time);
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
    // Standing of the player with the Authority and the traders (-100..100), and the Authority's purse.
    [[nodiscard]] f64 reputation() const { return m_reputation; }
    [[nodiscard]] bool hostile() const;
    [[nodiscard]] i64 treasury() const { return m_treasury; }
    // Open, accepted and recently closed contracts (ADR-031).
    [[nodiscard]] const std::vector<Contract>& contracts() const { return m_contracts; }
    // One line for the UI and the journal: "20 t de Agua a Arenmir II", "abatir al pirata ...".
    [[nodiscard]] Message describe(const Contract& contract) const;
    // The system's bank and the traders' mutual insurer (ADR-033), and owners waiting for a new ship.
    [[nodiscard]] const BankLedger& bank() const { return m_bank; }
    [[nodiscard]] const MutualLedger& mutual() const { return m_mutual; }
    [[nodiscard]] const std::vector<ShipBuyer>& buyers() const { return m_buyers; }
    // The mutual's current hull premium per ship-hour.
    [[nodiscard]] f64 premiumPerHour() const;
    // What the bank expects one more trader to earn per hour, after taxes, repairs and wages and before
    // premiums and debt: what traders earned lately, diluted by the ships added since.
    [[nodiscard]] f64 expectedEarningsPerHour() const;
    // A trader's net worth: credits on board, savings and cargo (at base prices), less its debt.
    [[nodiscard]] i64 traderWorth(const World& world, EntityId ship) const;
    [[nodiscard]] const SandboxConfig& config() const { return m_config; }
    [[nodiscard]] bool tactical() const;

    // The player's company (ADR-037). Every ship of the player's faction belongs to it and spends from its
    // account; the one the player flies is playerShip(), the others have a FleetBrain.
    [[nodiscard]] const CompanyBooks& company() const { return m_company; }
    [[nodiscard]] Wallet& account() { return m_company.account; }
    [[nodiscard]] const std::vector<EntityId>& fields() const { return m_fields; }
    [[nodiscard]] bool isPlayerShip(const World& world, EntityId ship) const;
    // What the company's hulls are worth, and what the bank lends against them (the borrowing base).
    [[nodiscard]] i64 fleetValue(const World& world) const;
    [[nodiscard]] i64 creditLimit(const World& world) const;
    // The mutual's premium per hour for a hull of `hullValue`, on the company's record (credibility-weighted
    // between the mutual's experience and the company's own).
    [[nodiscard]] f64 companyPremiumPerHour(i64 hullValue) const;
    // The account, the hulls and the cargo on board (at base prices), less the debt.
    [[nodiscard]] i64 companyWorth(const World& world) const;
    // What the company's ships carry, at base prices.
    [[nodiscard]] i64 companyCargoValue(const World& world) const;

private:
    EntityId spawnShip(World& world, SimTime now, Rng& rng, std::string name, u32 faction, u32 shipClass,
                       EntityId port);
    // At `port` (a random one if invalid), named after `serial` unless `name` is given.
    EntityId spawnHauler(World& world, SimTime now, Rng& rng, u32 serial, std::string name = {},
                         EntityId port = {});
    [[nodiscard]] std::string haulerName(Rng& rng, u32 serial) const;
    EntityId spawnPirate(World& world, SimTime now, Rng& rng, u32 serial);
    void setupMarkets(World& world);
    void planHaulerTrip(World& world, EntityId ship, HaulerBrain& brain, ShipControl& control, SimTime now,
                        Rng& rng);
    struct Delivery {
        EntityId port;
        GoodId good = 0;
        f64 tonnes = 0.0;
    };
    // A trader's next trip, judged with what `prices` knows, `budget` credits to spend and the cargo already
    // on its way (`inflight`): where to, and what to load here first.
    struct TripPlan {
        EntityId destination;
        GoodId load = 0;
        u32 tonnes = 0;
        bool reposition = false; // flying empty to where a known bargain is
        bool exploration = false;
    };
    [[nodiscard]] TripPlan planTrip(const World& world, EntityId ship, const PriceBook& prices,
                                    const std::vector<Delivery>& inflight, i64 budget, bool contracts,
                                    SimTime now, Rng& rng) const;
    // What `tonnes` of `good` would fetch at a known market, after the deliveries on their way there, and
    // with an open supply contract's reward if `contracts` and the load covers it.
    [[nodiscard]] f64 saleValue(const PortPrices& known, GoodId good, f64 tonnes,
                                const std::vector<Delivery>& inflight, bool contracts) const;
    void sellCargo(World& world, EntityId ship, EntityId port, SimTime now);
    // A trader arriving at a port hands over what its supply contracts there ask for.
    void deliverTraderContracts(World& world, EntityId ship, EntityId port, SimTime now);
    void addInflight(EntityId port, GoodId good, f64 tonnes);
    void addDanger(EntityId port, SimTime now);
    void onTradeCommand(const TradeCommand& command, const TickContext& context);
    // The company and its fleet (Game/Sandbox/SandboxFleet.cpp, ADR-037).
    void onMineCommand(const MineCommand& command, const TickContext& context);
    void onBuyShipCommand(const BuyShipCommand& command, const TickContext& context);
    void onSellShipCommand(const SellShipCommand& command, const TickContext& context);
    void onFleetOrderCommand(const FleetOrderCommand& command, const TickContext& context);
    void onFlagshipCommand(const FlagshipCommand& command, const TickContext& context);
    void onLoanCommand(const LoanCommand& command, const TickContext& context);
    void onInsureCommand(const InsureCommand& command, const TickContext& context);
    void updateMining(const TickContext& context);
    void updateFleet(const TickContext& context);
    // Wages, premiums and the debt of the company, every minute; an account overdrawn for too long loses a
    // ship to the bank.
    void updateCompany(const TickContext& context);
    EntityId spawnCompanyShip(World& world, SimTime now, u32 shipClass, EntityId port);
    // Whose purse pays for a ship: the company's account for the player's ships, its own Wallet otherwise.
    [[nodiscard]] Wallet* walletOf(World& world, EntityId ship);
    // A company ship was destroyed or captured: the mutual pays the insured hull.
    void onCompanyShipLost(World& world, EntityId ship, SimTime now);
    // Removes a company ship sold for `price` (its cargo sold at `port` if it is a market).
    void sellCompanyShip(World& world, EntityId ship, i64 price, EntityId port, SimTime now);
    // After the collateral shrank: the debt above the new borrowing base is due now.
    void enforceBorrowingBase(World& world, SimTime now);
    void startMining(World& world, EntityId ship, EntityId field);
    // Sells what the hold carries at the port it is docked at, for the company.
    void sellFleetCargo(World& world, EntityId ship, EntityId port, SimTime now);
    // The best known port to sell `tonnes` of `good` at (invalid if none): value per second of the whole
    // cycle, the trip there, back to `returnTo` (if valid) and `workSeconds` (a miner refilling its hold).
    [[nodiscard]] EntityId bestMarket(const World& world, EntityId ship, GoodId good, f64 tonnes, SimTime now,
                                      EntityId except = {}, EntityId returnTo = {},
                                      f64 workSeconds = 0.0) const;
    void fleetMine(World& world, EntityId ship, FleetBrain& brain, SimTime now);
    void fleetTrade(World& world, EntityId ship, FleetBrain& brain, SimTime now, u64 worldSeed);
    void fleetEscort(World& world, EntityId ship, FleetBrain& brain, SimTime now);
    // Sends a company ship to `target` (a body or a ship) and marks it travelling.
    void sendTo(World& world, EntityId ship, FleetBrain& brain, EntityId target);
    void onBoardCommand(const BoardCommand& command, const TickContext& context);
    void onContractCommand(const ContractCommand& command, const TickContext& context);
    void updateContracts(const TickContext& context);
    void closeContract(Contract& contract, ContractState state, SimTime now);
    // A wanted pirate was destroyed or captured: completes (by the player, accepted) or cancels its bounties.
    void settleBounty(EntityId pirate, bool byPlayer, SimTime now);
    void updatePayroll(const TickContext& context);
    // Premiums, loans, savings, the bank's and the mutual's experience and the purchase of new ships, every
    // minute (ADR-033).
    void updateFinance(const TickContext& context);
    void reviewShipPurchase(World& world, SimTime now);
    // A trader leaves the business (ship lost or repossessed): its credits on board, its savings and
    // `proceeds` (insurance claim or hull sale) pay its debt first; returns what is left, as a deposit.
    i64 settleEstate(World& world, EntityId ship, i64 proceeds);
    // A trader's ship was destroyed or captured: the mutual pays its hull and the owner may buy another.
    void onTraderLost(World& world, EntityId ship, SimTime now);
    // A bankrupt trader's ship is sold by the bank.
    void repossess(World& world, EntityId ship, SimTime now);
    // Buys a hull for an owner with `equity` (credits in hand): the bank lends what the down payment does
    // not cover. Returns the new ship.
    EntityId buyHauler(World& world, SimTime now, std::string name, i64 equity, i64 loan);
    [[nodiscard]] i64 debtServicePerHour(i64 loan) const;
    // Before a trader plans a trip: its savings, and credit against its hull if it is short of working
    // capital.
    void drawFunds(World& world, EntityId ship, Wallet& wallet);
    // Whether `faction`'s sensor picture has `ship` identified (a witness that can name it).
    [[nodiscard]] bool witnessedBy(u32 faction, EntityId ship) const;
    EntityId spawnPatrol(World& world, SimTime now, Rng& rng, u32 serial);
    void updatePatrols(const TickContext& context);
    void updateAuthority(const TickContext& context);
    void changeReputation(f64 delta);
    void payBounty(const std::string& name, SimTime now);
    void collectTax(i64 tax);
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
    void addJournal(SimTime time, Message text, JournalKind kind = JournalKind::Player);
    // An entity's name as a journal argument (a body's title, such as "Estación", is translated).
    [[nodiscard]] Message named(const World& world, EntityId entity) const;
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
    i64 m_treasury = 0;
    f64 m_reputation = 0.0;
    std::vector<Offense> m_offenses;
    std::vector<DistressCall> m_distress;
    std::vector<Contract> m_contracts;
    u32 m_nextContractId = 1;
    BankLedger m_bank;
    MutualLedger m_mutual;
    ExperienceRate m_earnings; // traders' operating earnings per ship-hour, as the bank sees them
    ExperienceRate m_fleet;    // ship-hours per hour over the same window: the fleet that earned them
    std::vector<ShipBuyer> m_buyers;
    SimTime m_nextNewcomer;       // outsiders are financed one at a time
    f64 m_announcedPremium = 0.0; // last premium in the news
    bool m_creditOpen = true;     // last credit conditions in the news
    PriceBook m_playerPrices;
    PriceBook m_traderPrices;
    std::vector<PortDanger> m_danger;
    std::vector<f64> m_initialStock;
    CompanyBooks m_company;
    // Derived from the World (rebuilt after loading, or on every Game.Haulers / Game.Fleet run).
    std::vector<Delivery> m_inflight;      // cargo the traders' ships are carrying to each port
    std::vector<Delivery> m_fleetInflight; // and the company's
    std::vector<EntityId> m_fields;
    std::vector<EntityId> m_ports;
    std::vector<EntityId> m_stations;
    std::vector<EntityId> m_planets;
    EntityId m_home;
    f64 m_exitRadius = 0.0; // beyond every planet's orbit: raiders leave the system through it
};

} // namespace gx
