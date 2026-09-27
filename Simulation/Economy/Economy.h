#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/Kernel/SystemScheduler.h"
#include "Simulation/World/EntityRegistry.h"

#include <string>
#include <vector>

// Minimal living economy (prompt §18, ADR-027). Markets hold stocks of goods; recipes turn inputs into
// outputs, populations consume, and the price of every good follows its stock: scarcity raises it, glut
// lowers it. Goods only move between markets in cargo holds, so what happens to ships (routes, losses) shows
// up in stocks and prices. No space dependency: the same model runs for a port, a colony or a whole
// aggregated system.
namespace gx {

class BinaryReader;
class BinaryWriter;
class Simulation;
struct TickContext;

using GoodId = u32;

struct GoodDef {
    std::string name;
    f64 basePrice = 0.0; // credits per tonne at the market's target stock
};

// Share of full output that production keeps when upkeep inputs (maintenance) are missing entirely.
inline constexpr f64 kUnmaintainedEfficiency = 0.35;
// Ports sell this much above the mid price and buy this much below it.
inline constexpr f64 kMarketSpread = 0.04;

// Mid price of one tonne at a stock level: the base price at the target stock, x2.83 when empty, x0.21 at
// 2.5 times the target (and flat beyond): base * 2^(1.5 * (1 - stock / target)).
[[nodiscard]] f64 unitPrice(f64 basePrice, f64 stock, f64 target);

struct RecipeInput {
    GoodId good = 0;
    f64 perUnit = 0.0;     // tonnes used per tonne produced
    bool essential = true; // essential: no input, no output. Upkeep: output drops to kUnmaintainedEfficiency

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("good", good);
        ar.io("perUnit", perUnit);
        ar.io("essential", essential);
    }
};

struct Recipe {
    GoodId output = 0;
    f64 rate = 0.0; // tonnes per hour at full efficiency
    std::vector<RecipeInput> inputs;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("output", output);
        ar.io("rate", rate);
        ar.io("inputs", inputs);
    }
};

// Population consumption.
struct Demand {
    GoodId good = 0;
    f64 rate = 0.0; // tonnes per hour

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("good", good);
        ar.io("rate", rate);
    }
};

struct MarketGood {
    GoodId good = 0;
    f64 stock = 0.0;
    f64 target = 0.0;   // stock at which the mid price is the base price
    f64 capacity = 0.0; // storage limit: production stops and the market stops buying when full
    // Cumulative statistics (economy inspector, conservation checks).
    f64 produced = 0.0;
    f64 consumed = 0.0; // by recipes and by the population
    f64 shortage = 0.0; // population demand that could not be met
    f64 boughtFromShips = 0.0;
    f64 soldToShips = 0.0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("good", good);
        ar.io("stock", stock);
        ar.io("target", target);
        ar.io("capacity", capacity);
        ar.io("produced", produced);
        ar.io("consumed", consumed);
        ar.io("shortage", shortage);
        ar.io("boughtFromShips", boughtFromShips);
        ar.io("soldToShips", soldToShips);
    }
};

// A port's market. Goods are kept sorted by id; only goods listed here are traded at the port.
struct Market {
    std::vector<MarketGood> goods;
    std::vector<Recipe> recipes; // run in order each economy tick
    std::vector<Demand> demands;

    [[nodiscard]] MarketGood* find(GoodId good);
    [[nodiscard]] const MarketGood* find(GoodId good) const;
    // Adds the good (sorted) if missing and returns it.
    MarketGood& ensure(GoodId good);

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("goods", goods);
        ar.io("recipes", recipes);
        ar.io("demands", demands);
    }
};

// One economy tick of a market: recipes in order, then the population's demand. Pure function of its inputs.
void runMarket(Market& market, f64 hours);

// Hourly rates implied by the recipes and demands (at full efficiency), for display and target stocks.
[[nodiscard]] f64 productionRate(const Market& market, GoodId good);
[[nodiscard]] f64 consumptionRate(const Market& market, GoodId good);

struct CargoItem {
    GoodId good = 0;
    u32 tonnes = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("good", good);
        ar.io("tonnes", tonnes);
    }
};

struct CargoHold {
    u32 capacity = 0;             // tonnes
    std::vector<CargoItem> items; // sorted by good, no empty entries

    [[nodiscard]] u32 used() const;
    [[nodiscard]] u32 space() const { return capacity - used(); }
    [[nodiscard]] u32 amount(GoodId good) const;
    void add(GoodId good, u32 tonnes);
    u32 remove(GoodId good, u32 tonnes); // returns what was removed

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("capacity", capacity);
        ar.io("items", items);
    }
};

struct Wallet {
    i64 credits = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("credits", credits);
    }
};

struct TradeResult {
    u32 tonnes = 0;
    i64 credits = 0; // paid to (buy) or received from (sell) the market, before tax
    i64 tax = 0;     // paid on top by the trader, for whoever levies it
};

// Ton by ton along the price curve, so large orders move the price against the trader. Buying stops when the
// market runs out of whole tonnes, the hold is full or the next tonne is unaffordable; selling stops when the
// market is full or the hold has no more of the good. The market must trade the good.
[[nodiscard]] i64 buyPrice(const MarketGood& good, f64 basePrice); // next tonne
[[nodiscard]] i64 sellPrice(const MarketGood& good, f64 basePrice);
// `taxRate`: share of each tonne's price the trader pays on top (buying) or leaves behind (selling).
TradeResult buyGoods(MarketGood& good, f64 basePrice, u32 tonnes, CargoHold& hold, Wallet& wallet,
                     f64 taxRate = 0.0);
TradeResult sellGoods(MarketGood& good, f64 basePrice, u32 tonnes, CargoHold& hold, Wallet& wallet,
                      f64 taxRate = 0.0);
// What buying `tonnes` would cost, without trading (hold and wallet limits ignored).
[[nodiscard]] TradeResult quoteBuy(const MarketGood& good, f64 basePrice, u32 tonnes);

// What a faction knows about prices: observations of port markets, with their age. Knowledge travels with
// ships (and bulletins); nobody reads a remote market directly.
struct PricePoint {
    GoodId good = 0;
    i64 buy = 0;  // the port sells at
    i64 sell = 0; // the port buys at
    f64 stockRatio = 0.0;
    f64 target = 0.0; // market depth: how far an order moves the price

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("good", good);
        ar.io("buy", buy);
        ar.io("sell", sell);
        ar.io("stockRatio", stockRatio);
        ar.io("target", target);
    }
};

struct PortPrices {
    EntityId port;
    SimTime observed;
    std::vector<PricePoint> prices; // sorted by good

    [[nodiscard]] const PricePoint* find(GoodId good) const;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("port", port);
        ar.io("observed", observed);
        ar.io("prices", prices);
    }
};

struct PriceBook {
    std::vector<PortPrices> ports; // sorted by port index

    void observe(EntityId port, const Market& market, const std::vector<GoodDef>& goods, SimTime now);
    [[nodiscard]] const PortPrices* find(EntityId port) const;
    // Takes every entry of `other` that is newer than ours (a bulletin). Returns how many were taken.
    u32 mergeNewer(const PriceBook& other);

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("ports", ports);
    }
};

// Runs every market on a fixed period, in parallel (markets are independent between trades).
class EconomySystem {
public:
    static void registerTypes(Simulation& simulation); // Market, CargoHold, Wallet

    void setGoods(std::vector<GoodDef> goods) { m_goods = std::move(goods); }
    [[nodiscard]] const std::vector<GoodDef>& goods() const { return m_goods; }
    [[nodiscard]] f64 basePrice(GoodId good) const {
        return good < m_goods.size() ? m_goods[good].basePrice : 0.0;
    }

    // grain: markets per job (each costs well under a microsecond).
    SystemId install(Simulation& simulation, SimDuration period, u32 grain = 512);
    void update(const TickContext& context);

private:
    std::vector<GoodDef> m_goods;
    u32 m_grain = 512;
};

} // namespace gx
