// Finance of the Sandbox (ADR-033): ships are bought with savings and bank credit, and insured by the
// traders' mutual. Every credit moves between explicit balances (wallets, TraderFinance, the bank's and the
// mutual's ledgers, or the outside world through the shipyard and the owners who come and go), so the
// ledgers' identities hold at every step.
#include "Game/Sandbox/Content.h"
#include "Game/Sandbox/Sandbox.h"

#include "Engine/Core/Hash.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Bodies/CelestialBody.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace gx {
namespace {

constexpr u64 kPurchaseStream = fnv1a64("sandbox.finance.purchase");

i64 perHours(f64 perHour, f64 hours) {
    return std::llround(perHour * hours);
}

} // namespace

f64 Sandbox::premiumPerHour() const {
    // A mutual does not seek profit: once its fund is large enough, members pay only the expected claims.
    const f64 loading = m_mutual.fund < content::kMutualTargetFund ? content::kPremiumLoading : 0.0;
    return m_mutual.premiumPerHour(content::kClaimCostPrior, content::kLossRatePriorHours, loading);
}

f64 Sandbox::expectedEarningsPerHour() const {
    // Margins narrow as ships compete for the same cargo: once the trade is saturated its total profit barely
    // grows with the fleet (measured, ADR-033). So what traders earned is diluted by the ships added since
    // (the fleet that earned it against the fleet with one more); a smaller fleet is not assumed to earn
    // more.
    const f64 perShip = m_earnings.estimate(content::kEarningsPrior, content::kEarningsPriorHours);
    const f64 earnedBy = m_fleet.estimate(static_cast<f64>(m_config.haulers), 1.0);
    const auto fleet = static_cast<f64>(m_simulation->world().components<HaulerBrain>().size());
    return perShip * std::min(1.0, earnedBy / (fleet + 1.0));
}

i64 Sandbox::debtServicePerHour(i64 loan) const {
    const auto principal = static_cast<f64>(loan);
    return std::llround(principal / content::kLoanTermHours + principal * content::kLoanRatePerHour);
}

i64 Sandbox::traderWorth(const World& world, EntityId ship) const {
    f64 worth = 0.0;
    if (const Wallet* wallet = world.components<Wallet>().tryGet(ship)) {
        worth += static_cast<f64>(wallet->credits);
    }
    if (const TraderFinance* finance = world.components<TraderFinance>().tryGet(ship)) {
        worth += static_cast<f64>(finance->deposit - finance->debt);
    }
    if (const CargoHold* hold = world.components<CargoHold>().tryGet(ship)) {
        for (const CargoItem& item : hold->items) {
            worth += item.tonnes * m_economy.basePrice(item.good);
        }
    }
    return std::llround(worth);
}

i64 Sandbox::settleEstate(World& world, EntityId ship, i64 proceeds) {
    TraderFinance* finance = world.components<TraderFinance>().tryGet(ship);
    Wallet* wallet = world.components<Wallet>().tryGet(ship);
    if (finance == nullptr) {
        return 0;
    }
    // The bank comes first: the payout or the sale of the hull (its security) and the credits on board pay
    // the debt, then the savings do; what nobody can pay is the bank's loss. Unpaid bills (negative credits)
    // come out of what is left.
    const i64 credits = wallet != nullptr ? wallet->credits : 0;
    i64 outside = proceeds + std::max<i64>(credits, 0);
    i64 debt = finance->debt;
    i64 deposit = finance->deposit;
    const i64 paid = std::min(debt, outside);
    m_bank.repay(paid);
    debt -= paid;
    outside -= paid;
    if (debt > 0) {
        const i64 offset = std::min(debt, deposit);
        m_bank.offset(offset);
        debt -= offset;
        deposit -= offset;
    }
    if (debt > 0) {
        m_bank.writeOff(debt);
    }
    outside += std::min<i64>(credits, 0);
    if (outside > 0) {
        m_bank.deposit(outside);
        deposit += outside;
    } else if (outside < 0) {
        const i64 owed = std::min(deposit, -outside);
        m_bank.withdraw(owed);
        deposit -= owed;
    }
    // The books are closed: nothing of this ship may be counted again before it leaves the World.
    *finance = {};
    if (wallet != nullptr) {
        wallet->credits = 0;
    }
    return deposit;
}

void Sandbox::onTraderLost(World& world, EntityId ship, SimTime now) {
    if (!m_config.finance || !world.components<TraderFinance>().contains(ship)) {
        return;
    }
    // The mutual pays the hull (as much as its fund allows): the owner's estate settles the debt, and what is
    // left waits at the bank for a new ship.
    const TraderFinance& finance = world.components<TraderFinance>().get(ship);
    const i64 insured = finance.hullValue;
    const i64 claim = m_mutual.pay(insured);
    if (claim < insured) {
        addJournal(now,
                   msg("Noticias: la Mutua de Fletadores solo puede pagar {} de los {} cr del casco de {}.",
                       number(claim), number(insured), named(world, ship)),
                   JournalKind::News);
    }
    const std::string name = nameOf(world, ship);
    const i64 equity = settleEstate(world, ship, claim);
    const auto minimum =
        static_cast<i64>(content::kMinDownPayment * static_cast<f64>(content::kHaulerHullPrice));
    if (equity >= minimum) {
        m_buyers.push_back({name, equity, now + m_config.haulerRespawnDelay, now + content::kBuyerPatience});
    } else {
        m_bank.withdraw(equity);
        m_stats.capitalOut += equity;
        ++m_stats.ownersRetired;
        addJournal(now, msg("Noticias: {} pierde su carguero y no puede comprar otro.", literal(name)),
                   JournalKind::News);
    }
}

void Sandbox::repossess(World& world, EntityId ship, SimTime now) {
    const TraderFinance* finance = world.components<TraderFinance>().tryGet(ship);
    if (!m_config.finance || finance == nullptr) {
        return;
    }
    // The bank sells the hull (outside the system) and keeps what its loan needs.
    const i64 debt = finance->debt;
    const i64 sale = std::llround(content::kHullRecovery * static_cast<f64>(finance->hullValue));
    const i64 lostBefore = m_bank.writtenOff;
    const i64 equity = settleEstate(world, ship, sale);
    m_bank.withdraw(equity);
    m_stats.capitalOut += equity;
    ++m_stats.repossessions;
    addJournal(now,
               debt > 0 ? msg("Noticias: el banco embarga el carguero de {} (deuda {} cr, pérdida {} cr).",
                              named(world, ship), number(debt), number(m_bank.writtenOff - lostBefore))
                        : msg("Noticias: {} vende su carguero.", named(world, ship)),
               JournalKind::News);
}

void Sandbox::drawFunds(World& world, EntityId ship, Wallet& wallet) {
    TraderFinance* finance = world.components<TraderFinance>().tryGet(ship);
    if (finance == nullptr) {
        return;
    }
    // Savings are at hand for the next load (the finance review puts back what is left).
    m_bank.withdraw(finance->deposit);
    wallet.credits += finance->deposit;
    finance->deposit = 0;
    // Short of working capital (or of credits to pay the crew): the bank lends against the hull, up to the
    // same loan-to-value it finances ships at. Illiquid is not insolvent: only a trader whose hull no longer
    // covers what it owes goes bankrupt.
    const auto collateral =
        static_cast<i64>((1.0 - content::kMinDownPayment) * static_cast<f64>(finance->hullValue));
    const i64 wanted = std::min(content::kWorkingCapital - wallet.credits, collateral - finance->debt);
    if (wanted > 0 && m_bank.canLend(wanted, content::kBankReserveRatio)) {
        m_bank.lend(wanted);
        wallet.credits += wanted;
        finance->debt += wanted;
        finance->instalment = std::max<i64>(1, static_cast<i64>(std::ceil(static_cast<f64>(finance->debt) /
                                                                          (content::kLoanTermHours * 60.0))));
    }
}

EntityId Sandbox::buyHauler(World& world, SimTime now, std::string name, i64 equity, i64 loan) {
    const i64 down = content::kHaulerHullPrice - loan;
    GX_CHECK(loan >= 0 && down >= 0 && equity >= down, "a ship bought without the money for it");
    m_bank.lend(loan);
    m_stats.shipyardPaid += content::kHaulerHullPrice;
    // New hulls are delivered at a station's yards.
    Rng rng = Rng::forStream(m_config.seed, kPurchaseStream, m_stats.shipsBought);
    const EntityId yard =
        m_stations.empty() ? m_home : m_stations[rng.uniformU32(static_cast<u32>(m_stations.size()))];
    const EntityId ship =
        spawnHauler(world, now, rng, static_cast<u32>(m_stats.spawns), std::move(name), yard);
    ++m_stats.spawns;
    ++m_stats.shipsBought;
    world.components<Wallet>().get(ship).credits = equity - down;
    TraderFinance& finance = world.components<TraderFinance>().get(ship);
    finance.debt = loan;
    finance.instalment =
        loan > 0 ? std::max<i64>(1, static_cast<i64>(
                                        std::ceil(static_cast<f64>(loan) / (content::kLoanTermHours * 60.0))))
                 : 0;
    finance.lastWorth = traderWorth(world, ship);
    return ship;
}

void Sandbox::updateFinance(const TickContext& context) {
    World& world = context.world;
    const SimTime now = context.now;
    const f64 seconds = context.dt.toSeconds();
    const f64 hours = seconds / 3600.0;
    m_earnings.decay(seconds, content::kExperienceHalfLife);
    m_fleet.decay(seconds, content::kExperienceHalfLife);
    m_mutual.losses.decay(seconds, content::kExperienceHalfLife);
    m_mutual.claimCost.decay(seconds, content::kExperienceHalfLife);

    const ComponentStore<HaulerBrain>& brains = world.components<HaulerBrain>();
    ComponentStore<TraderFinance>& books = world.components<TraderFinance>();
    ComponentStore<Wallet>& wallets = world.components<Wallet>();
    const f64 premiumRate = premiumPerHour() / static_cast<f64>(content::kHaulerHullPrice); // per cr insured
    const f64 depositRate = m_bank.depositRate(content::kLoanRatePerHour, content::kDepositRatePerHour,
                                               content::kDepositPassThrough);
    for (const EntityId ship : brains.entities()) {
        TraderFinance* finance = books.tryGet(ship);
        Wallet* wallet = wallets.tryGet(ship);
        if (finance == nullptr || wallet == nullptr) {
            continue;
        }
        // What trade earned since the last review, after taxes, repairs and wages: the bank's evidence.
        m_earnings.add(static_cast<f64>(traderWorth(world, ship) - finance->lastWorth), hours);

        const i64 premium = perHours(premiumRate * static_cast<f64>(finance->hullValue), hours);
        wallet->credits -= premium;
        m_mutual.collect(premium, hours);
        m_stats.premiumsPaid += premium;
        if (finance->debt > 0) {
            const i64 interest = perHours(static_cast<f64>(finance->debt) * content::kLoanRatePerHour, hours);
            const i64 principal = std::min(finance->instalment, finance->debt);
            wallet->credits -= interest + principal;
            m_bank.receiveInterest(interest);
            m_bank.repay(principal);
            finance->debt -= principal;
        }
        if (finance->deposit > 0) {
            const i64 interest = perHours(static_cast<f64>(finance->deposit) * depositRate, hours);
            finance->deposit += interest;
            m_bank.creditInterest(interest);
        }
        // Credits beyond the reserve pay the debt off early (it costs more than savings earn), then go to the
        // bank; a trader short of credits draws on its savings.
        i64 excess = wallet->credits - content::kTraderCashReserve;
        if (excess > 0) {
            const i64 prepaid = std::min(excess, finance->debt);
            m_bank.repay(prepaid);
            finance->debt -= prepaid;
            excess -= prepaid;
            m_bank.deposit(excess);
            finance->deposit += excess;
            wallet->credits = content::kTraderCashReserve;
        } else if (excess < 0 && finance->deposit > 0) {
            const i64 drawn = std::min(finance->deposit, -excess);
            m_bank.withdraw(drawn);
            finance->deposit -= drawn;
            wallet->credits += drawn;
        }
        finance->instalment = finance->debt > 0 ? finance->instalment : 0;
        finance->lastWorth = traderWorth(world, ship);
    }
    m_fleet.add(static_cast<f64>(brains.size()) * hours, hours);

    // The price of risk makes the news when it moves.
    const f64 premium = premiumPerHour();
    if (m_announcedPremium <= 0.0 ||
        std::abs(premium - m_announcedPremium) >= content::kPremiumNewsChange * m_announcedPremium) {
        if (m_announcedPremium > 0.0) {
            addJournal(
                now,
                premium > m_announcedPremium
                    ? msg("Noticias: la Mutua de Fletadores sube la prima del casco a {} cr/h por carguero.",
                          number(premium, 0))
                    : msg("Noticias: la Mutua de Fletadores baja la prima del casco a {} cr/h por carguero.",
                          number(premium, 0)),
                JournalKind::News);
        }
        m_announcedPremium = premium;
    }
    reviewShipPurchase(world, now);
    m_stats.peakHaulers = std::max<u64>(m_stats.peakHaulers, brains.size());
}

void Sandbox::reviewShipPurchase(World& world, SimTime now) {
    // Owners whose patience ran out leave the system with their equity.
    for (auto it = m_buyers.begin(); it != m_buyers.end();) {
        if (now < it->giveUpAt) {
            ++it;
            continue;
        }
        m_bank.withdraw(it->equity);
        m_stats.capitalOut += it->equity;
        ++m_stats.ownersRetired;
        addJournal(now,
                   msg("Noticias: {} no consigue crédito para otro carguero y abandona el sistema.",
                       literal(it->name)),
                   JournalKind::News);
        it = m_buyers.erase(it);
    }
    const ComponentStore<HaulerBrain>& brains = world.components<HaulerBrain>();
    if (brains.size() >= m_config.fleetCap()) {
        return;
    }
    // One purchase per review at most. A ship is worth buying if it is expected to earn more than its
    // premium, and the bank lends if that surplus covers the debt service with a margin (its debt service
    // coverage ratio, DSCR) and it has the cash.
    const f64 earnings = expectedEarningsPerHour();
    const f64 premium = premiumPerHour();
    const auto viable = [&](i64 loan) {
        const f64 surplus = earnings - premium;
        return surplus > 0.0 &&
               surplus >= content::kRequiredCoverage * static_cast<f64>(debtServicePerHour(loan));
    };
    const auto financeable = [&](i64 loan) {
        return viable(loan) && m_bank.canLend(loan, content::kBankReserveRatio);
    };
    const auto minimumDown =
        static_cast<i64>(content::kMinDownPayment * static_cast<f64>(content::kHaulerHullPrice));
    // The loan an owner with `equity` needs: everything goes into the hull (working capital can be borrowed
    // against it later), but the bank lends at most its loan-to-value.
    const auto loanFor = [&](i64 equity) {
        return content::kHaulerHullPrice - std::clamp(equity, minimumDown, content::kHaulerHullPrice);
    };
    bool bought = false;

    // 1. Owners who lost a ship, oldest first: their insurance payout is the down payment.
    const auto ready = std::find_if(m_buyers.begin(), m_buyers.end(), [&](const ShipBuyer& buyer) {
        return now >= buyer.readyAt && financeable(loanFor(buyer.equity));
    });
    if (ready != m_buyers.end()) {
        const ShipBuyer buyer = *ready;
        const i64 loan = loanFor(buyer.equity);
        m_buyers.erase(ready);
        m_bank.withdraw(buyer.equity);
        buyHauler(world, now, buyer.name, buyer.equity, loan);
        ++m_stats.shipsByReturningOwners;
        addJournal(now,
                   loan > 0 ? msg("Noticias: {} vuelve con un carguero nuevo (crédito de {} cr).",
                                  literal(buyer.name), number(loan))
                            : msg("Noticias: {} vuelve con un carguero nuevo.", literal(buyer.name)),
                   JournalKind::News);
        bought = true;
    }

    // Owners who lost a ship keep their place: others buy only into the room they leave.
    const bool room = brains.size() + m_buyers.size() < m_config.fleetCap();

    // 2. The trader with most savings, if they pay a whole hull and its working capital: a sister ship.
    if (!bought && room && viable(0)) {
        EntityId richest;
        i64 savings = content::kHaulerHullPrice + content::kWorkingCapital - 1;
        for (const EntityId ship : brains.entities()) {
            const TraderFinance* finance = world.components<TraderFinance>().tryGet(ship);
            if (finance != nullptr && finance->deposit > savings) {
                richest = ship;
                savings = finance->deposit;
            }
        }
        if (richest.isValid()) {
            const i64 invested = content::kHaulerHullPrice + content::kWorkingCapital;
            TraderFinance& parent = world.components<TraderFinance>().get(richest);
            parent.deposit -= invested;
            parent.lastWorth -= invested; // a transfer, not a loss of the trade
            m_bank.withdraw(invested);
            const std::string parentName = nameOf(world, richest);
            Rng rng = Rng::forStream(m_config.seed, kPurchaseStream ^ 1u, m_stats.shipsBought);
            std::string name =
                std::format("{}-{}", parentName.substr(0, parentName.rfind('-')), 10 + rng.uniformU32(90));
            const EntityId ship = buyHauler(world, now, std::move(name), invested, 0);
            ++m_stats.shipsByExpansion;
            addJournal(now,
                       msg("Noticias: {} invierte sus ahorros en un segundo carguero, {}.",
                           named(world, richest), named(world, ship)),
                       JournalKind::News);
            bought = true;
        }
    }

    // 3. A newcomer with outside savings, if the business looks good enough to lend it the rest.
    // A newcomer puts down the least the bank accepts and keeps the rest of its savings to trade with.
    const i64 loan = content::kHaulerHullPrice - minimumDown;
    if (!bought && room && now >= m_nextNewcomer && financeable(loan)) {
        m_nextNewcomer = now + content::kNewcomerInterval;
        Rng rng = Rng::forStream(m_config.seed, kPurchaseStream ^ 2u, m_stats.shipsBought);
        m_stats.capitalIn += content::kNewcomerSavings;
        const EntityId ship = buyHauler(world, now, haulerName(rng, static_cast<u32>(m_stats.spawns)),
                                        content::kNewcomerSavings, loan);
        ++m_stats.shipsByOutsiders;
        addJournal(now,
                   msg("{} llega al sistema con un carguero financiado por el banco.", named(world, ship)),
                   JournalKind::Traffic);
        bought = true;
    }

    // Credit conditions make the news when the business stops (or starts again) paying for new ships.
    // With some hysteresis, so the news does not flicker around the threshold: once open, it stays open
    // while the surplus still covers the debt service at all.
    const f64 surplus = earnings - premium;
    const bool open =
        m_creditOpen ? surplus > 0.0 && surplus >= static_cast<f64>(debtServicePerHour(loan)) : viable(loan);
    if (open != m_creditOpen) {
        m_creditOpen = open;
        addJournal(
            now,
            open ? msg("Noticias: el banco vuelve a financiar cargueros.")
                 : msg("Noticias: el banco deja de financiar cargueros: se espera que ganen {} cr/h, {} tras "
                       "la prima, y la deuda costaría {}.",
                       number(earnings, 0), number(earnings - premium, 0), number(debtServicePerHour(loan))),
            JournalKind::News);
    }
}

} // namespace gx
