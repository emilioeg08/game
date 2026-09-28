#include "Simulation/Economy/Economy.h"

#include "Engine/Core/Assert.h"
#include "Engine/Jobs/JobSystem.h"
#include "Simulation/Kernel/Simulation.h"

#include <algorithm>
#include <cmath>

namespace gx {
namespace {

constexpr f64 kPriceSlope = 1.5;    // price doubles every 2/3 of the target stock that goes missing
constexpr f64 kMaxStockRatio = 2.5; // the price stops falling beyond this glut

bool portLess(EntityId a, EntityId b) {
    return a.index != b.index ? a.index < b.index : a.generation < b.generation;
}

} // namespace

f64 unitPrice(f64 basePrice, f64 stock, f64 target) {
    if (target <= 0.0) {
        return basePrice;
    }
    const f64 ratio = std::clamp(stock / target, 0.0, kMaxStockRatio);
    return basePrice * std::exp2(kPriceSlope * (1.0 - ratio));
}

MarketGood* Market::find(GoodId good) {
    const auto it = std::lower_bound(goods.begin(), goods.end(), good,
                                     [](const MarketGood& g, GoodId id) { return g.good < id; });
    return it != goods.end() && it->good == good ? &*it : nullptr;
}

const MarketGood* Market::find(GoodId good) const {
    return const_cast<Market*>(this)->find(good);
}

MarketGood& Market::ensure(GoodId good) {
    const auto it = std::lower_bound(goods.begin(), goods.end(), good,
                                     [](const MarketGood& g, GoodId id) { return g.good < id; });
    if (it != goods.end() && it->good == good) {
        return *it;
    }
    MarketGood created;
    created.good = good;
    return *goods.insert(it, created);
}

void runMarket(Market& market, f64 hours) {
    if (hours <= 0.0) {
        return;
    }
    for (const Recipe& recipe : market.recipes) {
        MarketGood* output = market.find(recipe.output);
        if (output == nullptr) {
            continue;
        }
        // Essential inputs cap the output; missing upkeep only lowers efficiency.
        f64 desired = recipe.rate * hours;
        for (const RecipeInput& input : recipe.inputs) {
            if (input.essential && input.perUnit > 0.0) {
                const MarketGood* stock = market.find(input.good);
                desired = std::min(desired, stock != nullptr ? stock->stock / input.perUnit : 0.0);
            }
        }
        f64 efficiency = 1.0;
        for (const RecipeInput& input : recipe.inputs) {
            if (!input.essential && input.perUnit > 0.0 && desired > 0.0) {
                const MarketGood* stock = market.find(input.good);
                const f64 supplied =
                    std::min(1.0, (stock != nullptr ? stock->stock : 0.0) / (input.perUnit * desired));
                efficiency = std::min(efficiency,
                                      kUnmaintainedEfficiency + (1.0 - kUnmaintainedEfficiency) * supplied);
            }
        }
        const f64 produced =
            std::clamp(desired * efficiency, 0.0, std::max(0.0, output->capacity - output->stock));
        if (produced <= 0.0) {
            continue;
        }
        for (const RecipeInput& input : recipe.inputs) {
            if (MarketGood* stock = market.find(input.good)) {
                const f64 used = std::min(stock->stock, input.perUnit * produced);
                stock->stock -= used;
                stock->consumed += used;
            }
        }
        output->stock += produced; // no insertion happened: the pointer is still valid
        output->produced += produced;
    }
    for (const Demand& demand : market.demands) {
        if (MarketGood* stock = market.find(demand.good)) {
            const f64 wanted = demand.rate * hours;
            const f64 taken = std::min(stock->stock, wanted);
            stock->stock -= taken;
            stock->consumed += taken;
            stock->shortage += wanted - taken;
        }
    }
}

f64 productionRate(const Market& market, GoodId good) {
    f64 rate = 0.0;
    for (const Recipe& recipe : market.recipes) {
        if (recipe.output == good) {
            rate += recipe.rate;
        }
    }
    return rate;
}

f64 consumptionRate(const Market& market, GoodId good) {
    f64 rate = 0.0;
    for (const Recipe& recipe : market.recipes) {
        for (const RecipeInput& input : recipe.inputs) {
            if (input.good == good) {
                rate += input.perUnit * recipe.rate;
            }
        }
    }
    for (const Demand& demand : market.demands) {
        if (demand.good == good) {
            rate += demand.rate;
        }
    }
    return rate;
}

u32 CargoHold::used() const {
    u32 total = 0;
    for (const CargoItem& item : items) {
        total += item.tonnes;
    }
    return total;
}

u32 CargoHold::amount(GoodId good) const {
    for (const CargoItem& item : items) {
        if (item.good == good) {
            return item.tonnes;
        }
    }
    return 0;
}

void CargoHold::add(GoodId good, u32 tonnes) {
    if (tonnes == 0) {
        return;
    }
    const auto it = std::lower_bound(items.begin(), items.end(), good,
                                     [](const CargoItem& item, GoodId id) { return item.good < id; });
    if (it != items.end() && it->good == good) {
        it->tonnes += tonnes;
    } else {
        items.insert(it, CargoItem{good, tonnes});
    }
}

u32 CargoHold::remove(GoodId good, u32 tonnes) {
    for (auto it = items.begin(); it != items.end(); ++it) {
        if (it->good == good) {
            const u32 taken = std::min(tonnes, it->tonnes);
            it->tonnes -= taken;
            if (it->tonnes == 0) {
                items.erase(it);
            }
            return taken;
        }
    }
    return 0;
}

i64 buyPrice(const MarketGood& good, f64 basePrice) {
    const f64 mid = unitPrice(basePrice, good.stock - 0.5, good.target);
    return std::max<i64>(1, std::llround(mid * (1.0 + kMarketSpread)));
}

i64 sellPrice(const MarketGood& good, f64 basePrice) {
    const f64 mid = unitPrice(basePrice, good.stock + 0.5, good.target);
    return std::max<i64>(0, std::llround(mid * (1.0 - kMarketSpread)));
}

TradeResult quoteBuy(const MarketGood& good, f64 basePrice, u32 tonnes) {
    MarketGood copy = good;
    TradeResult result;
    while (result.tonnes < tonnes && copy.stock >= 1.0) {
        result.credits += buyPrice(copy, basePrice);
        copy.stock -= 1.0;
        ++result.tonnes;
    }
    return result;
}

TradeResult buyGoods(MarketGood& good, f64 basePrice, u32 tonnes, CargoHold& hold, Wallet& wallet,
                     f64 taxRate) {
    TradeResult result;
    const u32 limit = std::min(tonnes, hold.space());
    while (result.tonnes < limit && good.stock >= 1.0) {
        const i64 price = buyPrice(good, basePrice);
        const i64 tax = std::llround(static_cast<f64>(price) * taxRate);
        if (price + tax > wallet.credits) {
            break;
        }
        wallet.credits -= price + tax;
        good.stock -= 1.0;
        good.soldToShips += 1.0;
        result.credits += price;
        result.tax += tax;
        ++result.tonnes;
    }
    hold.add(good.good, result.tonnes);
    return result;
}

TradeResult sellGoods(MarketGood& good, f64 basePrice, u32 tonnes, CargoHold& hold, Wallet& wallet,
                      f64 taxRate) {
    TradeResult result;
    const u32 limit = std::min(tonnes, hold.amount(good.good));
    while (result.tonnes < limit && good.stock + 1.0 <= good.capacity) {
        const i64 price = sellPrice(good, basePrice);
        const i64 tax = std::llround(static_cast<f64>(price) * taxRate);
        wallet.credits += price - tax;
        good.stock += 1.0;
        good.boughtFromShips += 1.0;
        result.credits += price;
        result.tax += tax;
        ++result.tonnes;
    }
    hold.remove(good.good, result.tonnes);
    return result;
}

const PricePoint* PortPrices::find(GoodId good) const {
    for (const PricePoint& point : prices) {
        if (point.good == good) {
            return &point;
        }
    }
    return nullptr;
}

void PriceBook::observe(EntityId port, const Market& market, const std::vector<GoodDef>& goods, SimTime now) {
    auto it = std::lower_bound(ports.begin(), ports.end(), port,
                               [](const PortPrices& p, EntityId id) { return portLess(p.port, id); });
    if (it == ports.end() || it->port != port) {
        PortPrices created;
        created.port = port;
        it = ports.insert(it, std::move(created));
    }
    it->observed = now;
    it->prices.clear();
    for (const MarketGood& good : market.goods) {
        const f64 base = good.good < goods.size() ? goods[good.good].basePrice : 0.0;
        it->prices.push_back({good.good, buyPrice(good, base), sellPrice(good, base),
                              good.target > 0.0 ? good.stock / good.target : 0.0, good.target});
    }
}

const PortPrices* PriceBook::find(EntityId port) const {
    const auto it = std::lower_bound(ports.begin(), ports.end(), port,
                                     [](const PortPrices& p, EntityId id) { return portLess(p.port, id); });
    return it != ports.end() && it->port == port ? &*it : nullptr;
}

u32 PriceBook::mergeNewer(const PriceBook& other) {
    u32 taken = 0;
    for (const PortPrices& entry : other.ports) {
        auto it = std::lower_bound(ports.begin(), ports.end(), entry.port,
                                   [](const PortPrices& p, EntityId id) { return portLess(p.port, id); });
        if (it == ports.end() || it->port != entry.port) {
            ports.insert(it, entry);
            ++taken;
        } else if (it->observed < entry.observed) {
            *it = entry;
            ++taken;
        }
    }
    return taken;
}

u32 extract(Deposit& deposit, u32 tonnes) {
    const u32 taken = std::min(tonnes, static_cast<u32>(std::max(0.0, std::floor(deposit.reserve))));
    deposit.reserve -= taken;
    deposit.extracted += taken;
    return taken;
}

void recover(Deposit& deposit, f64 hours) {
    const f64 added =
        std::clamp(deposit.recovery * hours, 0.0, std::max(0.0, deposit.size - deposit.reserve));
    deposit.reserve += added;
    deposit.recovered += added;
}

void EconomySystem::registerTypes(Simulation& simulation) {
    World& world = simulation.world();
    world.registerComponent<Market>("Economy.Market");
    world.registerComponent<CargoHold>("Economy.CargoHold");
    world.registerComponent<Wallet>("Economy.Wallet");
    world.registerComponent<Deposit>("Economy.Deposit");
}

SystemId EconomySystem::install(Simulation& simulation, SimDuration period, u32 grain) {
    GX_CHECK(!m_goods.empty(), "EconomySystem::install needs the goods table");
    GX_CHECK(grain > 0, "economy grain must be positive");
    m_grain = grain;
    return simulation.addSystem(
        {"Simulation.Economy", TickPhase::Simulation, period, {}, [this](const TickContext& c) {
             update(c);
         }});
}

void EconomySystem::update(const TickContext& context) {
    ComponentStore<Market>& markets = context.world.components<Market>();
    const auto count = static_cast<u32>(markets.size());
    const f64 hours = context.dt.toSeconds() / 3600.0;
    if (count == 0 || hours <= 0.0) {
        return;
    }
    const std::span<Market> values = markets.values();
    context.jobs.parallelFor(
        count, m_grain,
        [&](u32 begin, u32 end) {
            for (u32 i = begin; i < end; ++i) {
                runMarket(values[i], hours);
            }
        },
        "Simulation.Economy.Markets");
    for (Deposit& deposit : context.world.components<Deposit>().values()) {
        recover(deposit, hours); // a handful of fields: serial
    }
}

} // namespace gx
