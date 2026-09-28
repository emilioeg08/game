#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/Economy/Economy.h"
#include "Simulation/Economy/Finance.h"
#include "Simulation/World/EntityRegistry.h"

// The player's company (ADR-037): the ships it owns besides the one the player flies, their standing orders,
// its books, and the commands that run it. The rules live in Game/Sandbox/SandboxFleet.cpp.
namespace gx {

struct ShipModules;

// Tonnes per hour a ship's mining lasers cut at their current health (0 without power).
[[nodiscard]] f64 miningRate(const ShipModules& modules);

// Player input: start or stop cutting a field with the ship's mining lasers (it must be among the rocks).
struct MineCommand {
    EntityId ship;
    EntityId field;
    bool on = true;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("ship", ship);
        ar.io("field", field);
        ar.io("on", on);
    }
};

// Player input: buy a ship at the yards of the station the player's ship is docked at. The bank lends the
// price less `downPayment` (paid from the company's account); a financed hull must be insured (ADR-037).
struct BuyShipCommand {
    EntityId ship; // the player's ship, docked at the station
    u32 shipClass = 0;
    i64 downPayment = 0;
    bool insured = true;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("ship", ship);
        ar.io("shipClass", shipClass);
        ar.io("downPayment", downPayment);
        ar.io("insured", insured);
    }
};

// Player input: sell a ship of the company, docked at a station, to the yards (its cargo is sold there).
struct SellShipCommand {
    EntityId ship;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("ship", ship);
    }
};

// Standing orders of a company ship the player does not fly:
//   Hold    stay where it is (docked, or stopped in space)
//   Dock    go to `site` (a port) and stay there
//   Mine    cut `site` (a field) until the hold is full, sell at `market` (invalid: the best known price)
//   Trade   buy and sell on its own, with what the company knows of prices
//   Escort  follow `site` (a company ship; invalid: the player's) and fight off raiders near it
enum class FleetOrder : u8 { Hold, Dock, Mine, Trade, Escort, Count };

[[nodiscard]] const char* toString(FleetOrder order);

// Player input: a standing order for a company ship (not the one the player flies).
struct FleetOrderCommand {
    EntityId ship;
    FleetOrder order = FleetOrder::Hold;
    EntityId site;
    EntityId market;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("ship", ship);
        ar.io("order", order);
        ar.io("site", site);
        ar.io("market", market);
    }
};

// Player input: take command of another company ship. The one the player leaves joins the fleet, holding.
struct FlagshipCommand {
    EntityId ship;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("ship", ship);
    }
};

// Player input: borrow from the bank against the fleet's hulls, and/or pay the debt back.
struct LoanCommand {
    i64 borrow = 0;
    i64 repay = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("borrow", borrow);
        ar.io("repay", repay);
    }
};

// Player input: insure a company ship with the traders' mutual, or cancel its cover.
struct InsureCommand {
    EntityId ship;
    bool insured = true;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("ship", ship);
        ar.io("insured", insured);
    }
};

// A ship with mining lasers at work: the field and the share of the next tonne already cut.
struct MiningControl {
    EntityId field;
    bool active = false;
    f64 progress = 0.0; // t

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("field", field);
        ar.io("active", active);
        ar.io("progress", progress);
    }
};

// A ship bought by the player's company: what it is worth (the bank's security, the mutual's cover), whether
// the mutual covers it, and what it has earned and cost (the fleet window).
struct OwnedShip {
    i64 hullValue = 0;
    bool insured = false;
    i64 income = 0;
    i64 expenses = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("hullValue", hullValue);
        ar.io("insured", insured);
        ar.io("income", income);
        ar.io("expenses", expenses);
    }
};

// What a company ship is doing about its order right now.
enum class FleetTask : u8 { Idle, Travelling, Mining, Fleeing, Escorting, Engaging, Count };

[[nodiscard]] const char* toString(FleetTask task);

// The hired captain of a company ship the player does not fly: carries out its standing order.
struct FleetBrain {
    FleetOrder order = FleetOrder::Hold;
    EntityId site;
    EntityId market;
    FleetTask task = FleetTask::Idle;
    SimTime nextDecision;
    u32 decisions = 0; // key for the per-decision random streams
    u32 trips = 0;
    u32 target = 0; // Engaging: the track it fights

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("order", order);
        ar.io("site", site);
        ar.io("market", market);
        ar.io("task", task);
        ar.io("nextDecision", nextDecision);
        ar.io("decisions", decisions);
        ar.io("trips", trips);
        ar.io("target", target);
    }
};

// Where the company's money went, since the start (credits). Every movement of the account is one of these,
// so the account always equals the starting credits plus
//   sales - purchases - taxes + contracts + bounties - repairs - wages - premiums - interest + claims
//   - shipsBought + shipsSold + borrowed - repaid
// (tested). A financed ship counts in shipsBought and in borrowed: the loan pays the yard.
struct CompanyTotals {
    i64 sales = 0;
    i64 purchases = 0;
    i64 taxes = 0;
    i64 contracts = 0;
    i64 bounties = 0;
    i64 repairs = 0;
    i64 wages = 0;
    i64 premiums = 0;
    i64 interest = 0;
    i64 claims = 0; // hull payouts received
    i64 shipsBought = 0;
    i64 shipsSold = 0;
    i64 borrowed = 0;
    i64 repaid = 0;
    i64 repairsInsured = 0; // paid by the mutual straight to the yards (not through the account)
    u64 tonnesMined = 0;
    u64 shipsLost = 0;
    u64 forcedSales = 0; // sold by the bank: the account stayed overdrawn

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("sales", sales);
        ar.io("purchases", purchases);
        ar.io("taxes", taxes);
        ar.io("contracts", contracts);
        ar.io("bounties", bounties);
        ar.io("repairs", repairs);
        ar.io("wages", wages);
        ar.io("premiums", premiums);
        ar.io("interest", interest);
        ar.io("claims", claims);
        ar.io("shipsBought", shipsBought);
        ar.io("shipsSold", shipsSold);
        ar.io("borrowed", borrowed);
        ar.io("repaid", repaid);
        ar.io("repairsInsured", repairsInsured);
        ar.io("tonnesMined", tonnesMined);
        ar.io("shipsLost", shipsLost);
        ar.io("forcedSales", forcedSales);
    }
};

// The player's company (ADR-037): one account for every ship the player owns, a credit line with the bank
// secured on the fleet's hulls (a borrowing base: the debt may not exceed kMinDownPayment's complement of
// what the hulls are worth), and the mutual's cover, priced on the company's own record as it builds up.
struct CompanyBooks {
    Wallet account;
    i64 debt = 0;
    i64 instalment = 0;     // principal due every minute
    bool drawing = false;   // the credit line is covering the account (reported once per episode)
    bool overdrawn = false; // short of cash beyond what the cargo on board is worth
    SimTime overdrawnSince;
    ExperienceRate claims; // the company's claims per insured hauler-hour: its credibility rating
    CompanyTotals totals;
    u32 shipsNamed = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("account", account);
        ar.io("debt", debt);
        ar.io("instalment", instalment);
        ar.io("drawing", drawing);
        ar.io("overdrawn", overdrawn);
        ar.io("overdrawnSince", overdrawnSince);
        ar.io("claims", claims);
        ar.io("totals", totals);
        ar.io("shipsNamed", shipsNamed);
    }
};

} // namespace gx
