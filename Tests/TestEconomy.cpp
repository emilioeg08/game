#include "Tests/TestFramework.h"

#include "Engine/Jobs/JobSystem.h"
#include "Simulation/Economy/Economy.h"
#include "Simulation/Economy/Finance.h"
#include "Simulation/Kernel/Simulation.h"

#include <cmath>
#include <vector>

using namespace gx;

namespace {

constexpr GoodId kOre = 0;
constexpr GoodId kMetals = 1;
constexpr GoodId kMachinery = 2;
constexpr GoodId kFood = 3;

MarketGood stocked(GoodId good, f64 stock, f64 target) {
    MarketGood result;
    result.good = good;
    result.stock = stock;
    result.target = target;
    result.capacity = target * 3.0;
    return result;
}

// Ore + machinery upkeep -> metals; the population eats.
Market refinery(f64 ore, f64 machinery) {
    Market market;
    market.ensure(kOre) = stocked(kOre, ore, 100.0);
    market.ensure(kMetals) = stocked(kMetals, 0.0, 100.0);
    market.ensure(kMachinery) = stocked(kMachinery, machinery, 100.0);
    market.recipes.push_back({kMetals, 60.0, {{kOre, 2.0, true}, {kMachinery, 0.1, false}}});
    return market;
}

} // namespace

GX_TEST(Economy, PriceFollowsStock) {
    GX_EXPECT_NEAR(unitPrice(100.0, 50.0, 50.0), 100.0, 1e-9);
    GX_EXPECT_NEAR(unitPrice(100.0, 0.0, 50.0), 100.0 * std::exp2(1.5), 1e-9); // empty: x2.83
    GX_EXPECT(unitPrice(100.0, 10.0, 50.0) > unitPrice(100.0, 20.0, 50.0));
    GX_EXPECT_NEAR(unitPrice(100.0, 125.0, 50.0), unitPrice(100.0, 500.0, 50.0), 1e-9); // flat beyond 2.5x
    MarketGood good = stocked(kOre, 100.0, 100.0);
    GX_EXPECT(buyPrice(good, 30.0) > sellPrice(good, 30.0)); // the spread
}

GX_TEST(Economy, EssentialInputsCapProductionAndUpkeepSetsEfficiency) {
    // Plenty of everything: one hour at full rate uses 2 ore per tonne of metal.
    Market full = refinery(1'000.0, 100.0);
    runMarket(full, 1.0);
    GX_EXPECT_NEAR(full.find(kMetals)->stock, 60.0, 1e-9);
    GX_EXPECT_NEAR(full.find(kOre)->stock, 1'000.0 - 120.0, 1e-9);
    GX_EXPECT_NEAR(full.find(kMachinery)->stock, 100.0 - 6.0, 1e-9);

    // 20 t of ore make at most 10 t of metal.
    Market starved = refinery(20.0, 100.0);
    runMarket(starved, 1.0);
    GX_EXPECT_NEAR(starved.find(kMetals)->stock, 10.0, 1e-9);
    GX_EXPECT_NEAR(starved.find(kOre)->stock, 0.0, 1e-9);

    // No machinery: the refinery limps on at kUnmaintainedEfficiency.
    Market unmaintained = refinery(1'000.0, 0.0);
    runMarket(unmaintained, 1.0);
    GX_EXPECT_NEAR(unmaintained.find(kMetals)->stock, 60.0 * kUnmaintainedEfficiency, 1e-9);

    // A full warehouse stops production.
    Market full2 = refinery(1'000.0, 100.0);
    full2.find(kMetals)->stock = full2.find(kMetals)->capacity;
    runMarket(full2, 1.0);
    GX_EXPECT_NEAR(full2.find(kOre)->stock, 1'000.0, 1e-9);
}

GX_TEST(Economy, UnmetDemandIsRecordedAsShortage) {
    Market town;
    town.ensure(kFood) = stocked(kFood, 30.0, 100.0);
    town.demands.push_back({kFood, 50.0});
    runMarket(town, 1.0);
    GX_EXPECT_NEAR(town.find(kFood)->stock, 0.0, 1e-9);
    GX_EXPECT_NEAR(town.find(kFood)->consumed, 30.0, 1e-9);
    GX_EXPECT_NEAR(town.find(kFood)->shortage, 20.0, 1e-9);
}

GX_TEST(Economy, TradesMoveThePriceAndConserveGoodsAndMoney) {
    MarketGood ore = stocked(kOre, 100.0, 100.0);
    CargoHold hold{50, {}};
    Wallet wallet{10'000};
    const TradeResult quote = quoteBuy(ore, 30.0, 20);
    const i64 firstPrice = buyPrice(ore, 30.0);
    const TradeResult bought = buyGoods(ore, 30.0, 20, hold, wallet);
    GX_EXPECT_EQ(bought.tonnes, 20u);
    GX_EXPECT_EQ(bought.credits, quote.credits);
    GX_EXPECT(bought.credits > firstPrice * 20); // each tonne dearer than the last
    GX_EXPECT_EQ(wallet.credits, 10'000 - bought.credits);
    GX_EXPECT_EQ(hold.amount(kOre), 20u);
    GX_EXPECT_NEAR(ore.stock, 80.0, 1e-9);

    // Limits: the hold's free space, then the money.
    const TradeResult tooMuch = buyGoods(ore, 30.0, 1'000, hold, wallet);
    GX_EXPECT_EQ(tooMuch.tonnes, 30u);
    GX_EXPECT_EQ(hold.space(), 0u);
    CargoHold big{1'000, {}};
    Wallet poor{100};
    const TradeResult broke = buyGoods(ore, 30.0, 1'000, big, poor);
    GX_EXPECT(broke.tonnes > 0 && broke.tonnes < 5);
    GX_EXPECT(poor.credits >= 0 && poor.credits < buyPrice(ore, 30.0));

    // Selling back returns less than was paid (spread and slippage), and stops when the market is full.
    const i64 before = wallet.credits;
    const TradeResult sold = sellGoods(ore, 30.0, 50, hold, wallet);
    GX_EXPECT_EQ(sold.tonnes, 50u);
    GX_EXPECT(sold.credits < bought.credits + tooMuch.credits);
    GX_EXPECT_EQ(wallet.credits, before + sold.credits);
    GX_EXPECT(hold.items.empty());
    MarketGood glut = stocked(kOre, 299.5, 100.0);
    CargoHold loaded{100, {{kOre, 10}}};
    GX_EXPECT_EQ(sellGoods(glut, 30.0, 10, loaded, wallet).tonnes, 0u);
    GX_EXPECT_EQ(loaded.amount(kOre), 10u);
}

GX_TEST(Economy, PriceBooksAgeAndMergeOnlyNewerNews) {
    const std::vector<GoodDef> goods = {
        {"ore", 30.0}, {"metals", 120.0}, {"machinery", 300.0}, {"food", 40.0}};
    const Market market = refinery(100.0, 100.0);
    const EntityId portA{1, 0};
    const EntityId portB{2, 0};
    PriceBook mine;
    PriceBook theirs;
    mine.observe(portA, market, goods, SimTime::epoch() + SimDuration::minutes(10));
    theirs.observe(portA, market, goods, SimTime::epoch() + SimDuration::minutes(5)); // older: ignored
    theirs.observe(portB, market, goods, SimTime::epoch() + SimDuration::minutes(7));
    GX_EXPECT_EQ(mine.mergeNewer(theirs), 1u);
    GX_REQUIRE(mine.find(portA) != nullptr && mine.find(portB) != nullptr);
    GX_EXPECT(mine.find(portA)->observed == SimTime::epoch() + SimDuration::minutes(10));
    const PricePoint* ore = mine.find(portB)->find(kOre);
    GX_REQUIRE(ore != nullptr);
    GX_EXPECT_NEAR(ore->stockRatio, 1.0, 1e-9);
    GX_EXPECT_NEAR(ore->target, 100.0, 1e-9);
}

GX_TEST(Economy, ParallelMarketsAreDeterministic) {
    const auto run = [](u32 workers) {
        JobSystem jobs(workers);
        Simulation simulation({.seed = 3}, jobs);
        EconomySystem::registerTypes(simulation);
        EconomySystem economy;
        economy.setGoods({{"ore", 30.0}, {"metals", 120.0}, {"machinery", 300.0}, {"food", 40.0}});
        economy.install(simulation, SimDuration::seconds(10), 16);
        for (u32 i = 0; i < 1'000; ++i) {
            const EntityId port = simulation.world().createEntity();
            Market market = refinery(50.0 + (i % 97), (i % 5) * 3.0);
            market.ensure(kFood) = stocked(kFood, 40.0 + (i % 13), 100.0);
            market.demands.push_back({kFood, 20.0 + (i % 7)});
            simulation.world().components<Market>().add(port, market);
        }
        simulation.runFor(SimDuration::minutes(30));
        return simulation.stateHash();
    };
    const u64 reference = run(0);
    GX_EXPECT_EQ(run(3), reference);
    GX_EXPECT_EQ(run(7), reference);
}

GX_TEST(Economy, ExperienceRateShrinksTowardsThePriorAndForgets) {
    ExperienceRate rate;
    GX_EXPECT_NEAR(rate.estimate(0.05, 40.0), 0.05, 1e-12); // no evidence: the prior
    rate.add(6.0, 60.0);                                    // 6 losses in 60 ship-hours: 0.1 per hour
    GX_EXPECT_NEAR(rate.estimate(0.05, 40.0), (6.0 + 2.0) / 100.0, 1e-12);
    GX_EXPECT_NEAR(rate.estimate(0.05, 0.0), 0.1, 1e-12); // no prior: the evidence alone
    rate.decay(7'200.0, 7'200.0);                         // one half-life: half the weight, same ratio
    GX_EXPECT_NEAR(rate.observed, 3.0, 1e-12);
    GX_EXPECT_NEAR(rate.exposure, 30.0, 1e-12);
    GX_EXPECT_NEAR(rate.estimate(0.05, 40.0), (3.0 + 2.0) / 70.0, 1e-12); // back towards the prior
}

GX_TEST(Economy, BankLedgerBalancesThroughEveryOperation) {
    BankLedger bank;
    bank.open(10'000);
    GX_EXPECT(bank.balanced());
    GX_EXPECT(bank.canLend(8'000, 0.2));
    bank.lend(8'000);
    bank.deposit(5'000);
    GX_EXPECT(bank.canLend(6'000, 0.2)); // 7,000 cash, 1,000 kept against deposits
    GX_EXPECT(!bank.canLend(6'001, 0.2));
    GX_EXPECT(bank.canLend(7'000, 0.0));
    bank.receiveInterest(160);
    bank.creditInterest(25);
    bank.repay(3'000);
    bank.offset(1'000); // a borrower's savings pay its own debt
    bank.withdraw(500);
    bank.writeOff(1'500);
    GX_EXPECT(bank.balanced());
    GX_EXPECT_EQ(bank.loans, 8'000 - 3'000 - 1'000 - 1'500);
    GX_EXPECT_EQ(bank.deposits, 5'000 + 25 - 1'000 - 500);
    GX_EXPECT_EQ(bank.equity(), 10'000 + 160 - 25 - 1'500);
    GX_EXPECT_EQ(bank.defaults, 1u);
    GX_EXPECT_EQ(bank.loansGranted, 1u);
}

GX_TEST(Economy, DepositsEarnOnlyWhatLoansPayFor) {
    BankLedger bank;
    bank.open(10'000);
    bank.deposit(20'000);
    GX_EXPECT_NEAR(bank.depositRate(0.02, 0.005, 0.5), 0.0, 1e-12); // nobody borrows the savings
    bank.lend(10'000);
    GX_EXPECT_NEAR(bank.depositRate(0.02, 0.005, 0.5), 0.02 * 0.5 * 0.5, 1e-12);
    bank.lend(15'000);
    GX_EXPECT_NEAR(bank.depositRate(0.02, 0.005, 0.5), 0.005, 1e-12); // capped
}

GX_TEST(Economy, MutualPaysWhatItHasAndPricesItsExperience) {
    MutualLedger mutual;
    mutual.open(5'000);
    mutual.collect(1'000, 10.0);
    GX_EXPECT_NEAR(mutual.premiumPerHour(500.0, 10.0, 0.25), 250.0 * 1.25,
                   1e-9);                   // 10 h clean: half the prior
    GX_EXPECT(mutual.cover(2'000));         // a repair
    GX_EXPECT_EQ(mutual.pay(6'000), 4'000); // a total loss larger than the fund: paid in part
    GX_EXPECT_EQ(mutual.fund, 0);
    GX_EXPECT(!mutual.cover(100)); // an empty fund covers no repairs
    GX_EXPECT(mutual.balanced());
    GX_EXPECT_EQ(mutual.claims, 1u);
    GX_EXPECT_EQ(mutual.claimsShort, 1u);
    GX_EXPECT_EQ(mutual.repairsPaid, 2'000);
    // The risk counts what was lost, paid or not: 2,000 + 6,000 + 100 over 10 insured hours.
    GX_EXPECT_NEAR(mutual.premiumPerHour(500.0, 10.0, 0.0), (8'100.0 + 5'000.0) / 20.0, 1e-9);
    GX_EXPECT_NEAR(mutual.losses.estimate(0.0, 0.0), 0.1, 1e-12);
}
