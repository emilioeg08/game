// The player's company and its fleet (ADR-037). Ships are bought at a station's yards with the company's
// account and bank credit, insured by the traders' mutual and crewed by hired captains who carry out
// standing orders: hold, dock, mine an asteroid field, trade, or escort the ship the player flies. Every
// credit moves between the company's account, the markets, the Authority, the bank's and the mutual's
// ledgers and the yards (outside the system), so the ledgers' identities keep holding.
#include "Game/Sandbox/Content.h"
#include "Game/Sandbox/Sandbox.h"
#include "Game/Sandbox/SandboxDetail.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Log.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Bodies/CelestialBody.h"
#include "Space/Ships/Modules.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace gx {
namespace {

using sandbox_detail::entityKey;
using sandbox_detail::resetOrders;

constexpr u64 kYardStream = fnv1a64("sandbox.fleet.yard");
constexpr u64 kFleetTradeStream = fnv1a64("sandbox.fleet.trade");
constexpr SimDuration kFleetDwell = SimDuration::seconds(20); // in port between trades
constexpr f64 kFieldSwitchReserve =
    0.25; // a miner moves to a field of the same kind this full, if its own is dry

i64 perHours(f64 perHour, f64 hours) {
    return std::llround(perHour * hours);
}

// Equal principal instalments over the loan term, paid every minute.
i64 instalmentFor(i64 debt) {
    return debt > 0 ? std::max<i64>(1, static_cast<i64>(std::ceil(static_cast<f64>(debt) /
                                                                  (content::kLoanTermHours * 60.0))))
                    : 0;
}

Message orderText(const World& world, const FleetBrain& brain) {
    const auto name = [&](EntityId entity) {
        const CelestialBody* body = world.components<CelestialBody>().tryGet(entity);
        return properName(body != nullptr ? body->name : std::string("?"));
    };
    switch (brain.order) {
    case FleetOrder::Hold:
        return msg("mantener la posición");
    case FleetOrder::Dock:
        return msg("atracar en {}", name(brain.site));
    case FleetOrder::Mine:
        return brain.market.isValid() ? msg("minar {} y vender en {}", name(brain.site), name(brain.market))
                                      : msg("minar {} y vender donde mejor paguen", name(brain.site));
    case FleetOrder::Trade:
        return msg("comerciar por su cuenta");
    case FleetOrder::Escort: {
        const ShipIdentity* ward =
            world.isAlive(brain.site) ? world.components<ShipIdentity>().tryGet(brain.site) : nullptr;
        return ward != nullptr ? msg("escoltar a {}", literal(ward->name)) : msg("escoltar tu nave");
    }
    case FleetOrder::Count:
        break;
    }
    return literal("?");
}

} // namespace

f64 miningRate(const ShipModules& modules) {
    // Every working laser cuts in proportion to its health.
    if (!hasPower(modules)) {
        return 0.0;
    }
    f64 rate = 0.0;
    for (const ShipModule& module : modules.modules) {
        if (module.type == ModuleType::Mining && module.functional()) {
            rate += content::kMiningRatePerModule * module.fraction();
        }
    }
    return rate;
}

const char* toString(FleetOrder order) {
    switch (order) {
    case FleetOrder::Hold:
        return "Hold";
    case FleetOrder::Dock:
        return "Dock";
    case FleetOrder::Mine:
        return "Mine";
    case FleetOrder::Trade:
        return "Trade";
    case FleetOrder::Escort:
        return "Escort";
    case FleetOrder::Count:
        break;
    }
    return "?";
}

const char* toString(FleetTask task) {
    switch (task) {
    case FleetTask::Idle:
        return "Idle";
    case FleetTask::Travelling:
        return "Travelling";
    case FleetTask::Mining:
        return "Mining";
    case FleetTask::Fleeing:
        return "Fleeing";
    case FleetTask::Escorting:
        return "Escorting";
    case FleetTask::Engaging:
        return "Engaging";
    case FleetTask::Count:
        break;
    }
    return "?";
}

// --- The company's books
// ------------------------------------------------------------------------------------

bool Sandbox::isPlayerShip(const World& world, EntityId ship) const {
    if (!world.isAlive(ship)) {
        return false;
    }
    const ShipIdentity* identity = world.components<ShipIdentity>().tryGet(ship);
    return identity != nullptr && identity->faction == content::kFactionPlayer;
}

Wallet* Sandbox::walletOf(World& world, EntityId ship) {
    if (isPlayerShip(world, ship)) {
        return &m_company.account;
    }
    return world.components<Wallet>().tryGet(ship);
}

i64 Sandbox::fleetValue(const World& world) const {
    i64 value = 0;
    for (const OwnedShip& owned : world.components<OwnedShip>().values()) {
        value += owned.hullValue;
    }
    return value;
}

i64 Sandbox::creditLimit(const World& world) const {
    // The same loan-to-value the bank finances hulls at (ADR-033): a borrowing base on the whole fleet.
    if (!m_config.finance) {
        return 0;
    }
    return static_cast<i64>((1.0 - content::kMinDownPayment) * static_cast<f64>(fleetValue(world)));
}

f64 Sandbox::companyPremiumPerHour(i64 hullValue) const {
    if (!m_config.finance) {
        return 0.0;
    }
    // Experience rating: the mutual's expected claims per hauler-hour, credibility-weighted with the
    // company's own record (Bühlmann: Z = n / (n + k), n its insured hauler-hours, k = kCredibilityHours). A
    // company that loses nothing pays less and less; one that loses ships, more.
    const f64 pool = m_mutual.claimCost.estimate(content::kClaimCostPrior, content::kLossRatePriorHours);
    const f64 own = m_company.claims.estimate(pool, content::kCredibilityHours);
    const f64 loading = m_mutual.fund < content::kMutualTargetFund ? content::kPremiumLoading : 0.0;
    return own * (1.0 + loading) * static_cast<f64>(hullValue) / static_cast<f64>(content::kHaulerHullPrice);
}

i64 Sandbox::companyWorth(const World& world) const {
    return m_company.account.credits + fleetValue(world) - m_company.debt + companyCargoValue(world);
}

i64 Sandbox::companyCargoValue(const World& world) const {
    f64 worth = 0.0;
    const ComponentStore<ShipIdentity>& identities = world.components<ShipIdentity>();
    for (usize i = 0; i < identities.size(); ++i) {
        if (identities.values()[i].faction != content::kFactionPlayer) {
            continue;
        }
        if (const CargoHold* hold = world.components<CargoHold>().tryGet(identities.entities()[i])) {
            for (const CargoItem& item : hold->items) {
                worth += item.tonnes * m_economy.basePrice(item.good);
            }
        }
    }
    return std::llround(worth);
}

EntityId Sandbox::spawnCompanyShip(World& world, SimTime now, u32 shipClass, EntityId port) {
    Rng rng = Rng::forStream(m_config.seed, kYardStream, m_company.shipsNamed);
    std::string name =
        std::format("{}-{}", content::kFleetNames[m_company.shipsNamed % content::kFleetNames.size()],
                    10 + rng.uniformU32(90));
    ++m_company.shipsNamed;
    const EntityId ship =
        spawnShip(world, now, rng, std::move(name), content::kFactionPlayer, shipClass, port);
    world.components<FleetBrain>().add(ship, {FleetOrder::Hold, {}, {}, FleetTask::Idle, now, 0, 0, 0});
    return ship;
}

void Sandbox::enforceBorrowingBase(World& world, SimTime now) {
    const i64 limit = creditLimit(world);
    if (m_company.debt <= limit) {
        return;
    }
    // The collateral shrank (a ship lost or sold): what the debt exceeds the borrowing base by is due now.
    const i64 due = m_company.debt - limit;
    m_company.account.credits -= due;
    m_bank.repay(due);
    m_company.debt -= due;
    m_company.totals.repaid += due;
    m_company.instalment = instalmentFor(m_company.debt);
    addJournal(now,
               msg("El banco reclama {} cr: tu deuda superaba lo que presta contra tu flota.", number(due)));
}

void Sandbox::sellCompanyShip(World& world, EntityId ship, i64 price, EntityId port, SimTime now) {
    if (port.isValid()) {
        sellFleetCargo(world, ship, port, now);
    }
    if (const CargoHold* hold = world.components<CargoHold>().tryGet(ship)) {
        for (const CargoItem& item : hold->items) {
            m_stats.cargoLost[item.good] += item.tonnes; // leaves the system with the ship
        }
    }
    m_company.account.credits += price;
    m_company.totals.shipsSold += price;
    world.destroyEntity(ship);
    enforceBorrowingBase(world, now);
}

void Sandbox::onCompanyShipLost(World& world, EntityId ship, SimTime now) {
    OwnedShip* owned = world.components<OwnedShip>().tryGet(ship);
    if (owned == nullptr) {
        return; // the courier the player started with: replaced for free
    }
    ++m_company.totals.shipsLost;
    const i64 hull = owned->hullValue;
    if (owned->insured && m_config.finance) {
        const i64 claim = m_mutual.pay(hull);
        m_company.claims.add(static_cast<f64>(hull), 0.0);
        m_company.account.credits += claim;
        m_company.totals.claims += claim;
        addJournal(now, msg("Pierdes {}. La Mutua de Fletadores paga {} cr por el casco.", named(world, ship),
                            number(claim)));
    } else {
        addJournal(now, msg("Pierdes {}. No estaba asegurada: sus {} cr se pierden con ella.",
                            named(world, ship), number(hull)));
    }
    owned->hullValue = 0; // no longer security for the debt
    enforceBorrowingBase(world, now);
}

void Sandbox::updateCompany(const TickContext& context) {
    World& world = context.world;
    const SimTime now = context.now;
    const f64 seconds = context.dt.toSeconds();
    const f64 hours = seconds / 3600.0;
    m_company.claims.decay(seconds, content::kExperienceHalfLife);

    // Crews every minute, and the premiums of the insured hulls.
    i64 wages = 0;
    i64 premiums = 0;
    f64 exposure = 0.0; // insured hauler-hours
    ComponentStore<OwnedShip>& owned = world.components<OwnedShip>();
    for (usize i = 0; i < owned.size(); ++i) {
        OwnedShip& ship = owned.values()[i];
        const ShipIdentity* identity = world.components<ShipIdentity>().tryGet(owned.entities()[i]);
        const i64 wage =
            identity != nullptr
                ? perHours(static_cast<f64>(content::kShipWagesPerMinute[identity->shipClass]) * 60.0, hours)
                : 0;
        const bool covered = ship.insured && m_config.finance && ship.hullValue > 0;
        const i64 premium = covered ? perHours(companyPremiumPerHour(ship.hullValue), hours) : 0;
        exposure +=
            covered ? static_cast<f64>(ship.hullValue) / static_cast<f64>(content::kHaulerHullPrice) * hours
                    : 0.0;
        ship.expenses += wage + premium;
        wages += wage;
        premiums += premium;
    }
    m_company.account.credits -= wages + premiums;
    m_company.totals.wages += wages;
    m_company.totals.premiums += premiums;
    if (exposure > 0.0) {
        m_mutual.collect(premiums, exposure);
        m_company.claims.add(0.0, exposure);
    }

    // The debt: interest on what is owed and the instalment of principal.
    if (m_company.debt > 0) {
        const i64 interest = perHours(static_cast<f64>(m_company.debt) * content::kLoanRatePerHour, hours);
        const i64 principal = std::min(m_company.instalment, m_company.debt);
        m_company.account.credits -= interest + principal;
        m_bank.receiveInterest(interest);
        m_bank.repay(principal);
        m_company.debt -= principal;
        m_company.totals.interest += interest;
        m_company.totals.repaid += principal;
    }
    m_company.instalment = m_company.debt > 0 ? m_company.instalment : 0;

    // Overdrawn: the bank's credit line covers it while the hulls allow (illiquid is not insolvent).
    bool drew = false;
    if (m_company.account.credits < 0 && m_config.finance) {
        const i64 draw = std::min(-m_company.account.credits, creditLimit(world) - m_company.debt);
        if (draw > 0 && m_bank.canLend(draw, content::kBankReserveRatio)) {
            m_bank.lend(draw);
            m_company.debt += draw;
            m_company.account.credits += draw;
            m_company.totals.borrowed += draw;
            m_company.instalment = instalmentFor(m_company.debt);
            drew = true;
            if (!m_company.drawing) {
                addJournal(
                    now, msg("El banco cubre tu descubierto con tu línea de crédito ({} cr).", number(draw)));
            }
        }
    }
    m_company.drawing = drew;
    // Short of cash beyond what the cargo on board will fetch: the bank waits kArrearsGrace, then sells.
    if (m_company.account.credits + companyCargoValue(world) >= 0) {
        m_company.overdrawn = false;
        return;
    }
    if (!m_company.overdrawn) {
        m_company.overdrawn = true;
        m_company.overdrawnSince = now;
        addJournal(now, msg("Tu cuenta está en descubierto ({} cr) y el banco no presta más. Si sigue así {} "
                            "minutos, se venderá una nave para cubrirlo.",
                            number(m_company.account.credits),
                            number(content::kArrearsGrace.toSeconds() / 60.0, 0)));
        return;
    }
    if (now - m_company.overdrawnSince < content::kArrearsGrace) {
        return;
    }
    // Still overdrawn: the most valuable ship of the fleet (never the one the player flies) is sold at the
    // price a forced sale fetches.
    EntityId sold;
    i64 value = 0;
    for (usize i = 0; i < owned.size(); ++i) {
        const EntityId ship = owned.entities()[i];
        if (ship != m_player && owned.values()[i].hullValue > value) {
            sold = ship;
            value = owned.values()[i].hullValue;
        }
    }
    m_company.overdrawnSince = now;
    if (!sold.isValid()) {
        return;
    }
    const i64 price = std::llround(content::kHullRecovery * static_cast<f64>(value));
    addJournal(now, msg("Tu cuenta sigue en descubierto: se vende {} por {} cr.", named(world, sold),
                        number(price)));
    ++m_company.totals.forcedSales;
    sellCompanyShip(world, sold, price, {}, now);
}

// --- Commands -------------------------------------------------------------------------------------------

void Sandbox::onBuyShipCommand(const BuyShipCommand& command, const TickContext& context) {
    World& world = context.world;
    const auto reject = [&](const char* reason, const char* message) {
        ++m_stats.commandsRejected;
        GX_LOG_WARN("Sandbox", "buy ship command rejected: {}", reason);
        addJournal(context.now, msg(message));
    };
    const EntityId port = command.ship == m_player ? dockedPort(world, command.ship) : EntityId{};
    const CelestialBody* body = world.components<CelestialBody>().tryGet(port);
    if (body == nullptr || body->kind != BodyKind::Station) {
        reject("not docked at a station", GX_TEXT("Los astilleros están en las estaciones: atraca en una."));
        return;
    }
    if (hostile()) {
        reject("hostile", GX_TEXT("La Autoridad no vende naves a quien considera hostil."));
        return;
    }
    if (std::find(content::kShipsForSale.begin(), content::kShipsForSale.end(), command.shipClass) ==
        content::kShipsForSale.end()) {
        reject("not for sale", GX_TEXT("Esa nave no está a la venta."));
        return;
    }
    if (world.components<FleetBrain>().size() >= content::kMaxFleet) {
        reject("fleet full", GX_TEXT("Tu flota ya tiene doce naves."));
        return;
    }
    const i64 price = content::kShipPrices[command.shipClass];
    if (command.downPayment < 0 || command.downPayment > price) {
        reject("invalid down payment", GX_TEXT("Entrada no válida."));
        return;
    }
    if (command.downPayment > m_company.account.credits) {
        reject("cannot pay", GX_TEXT("No tienes créditos para la entrada."));
        return;
    }
    const i64 loan = price - command.downPayment;
    if (loan > 0) {
        if (!m_config.finance) {
            reject("no bank", GX_TEXT("No hay banco en este sistema: hay que pagar la nave entera."));
            return;
        }
        const auto limit =
            static_cast<i64>((1.0 - content::kMinDownPayment) * static_cast<f64>(fleetValue(world) + price));
        if (m_company.debt + loan > limit) {
            reject("over the borrowing base", GX_TEXT("El banco no presta tanto: tu deuda superaría las tres "
                                                      "cuartas partes del valor de tu flota."));
            return;
        }
        if (!m_bank.canLend(loan, content::kBankReserveRatio)) {
            reject("bank short of cash", GX_TEXT("El banco no tiene fondos para prestarte ahora."));
            return;
        }
    }
    const bool insured = m_config.finance && (command.insured || loan > 0); // the bank requires cover

    m_company.account.credits -= command.downPayment;
    if (loan > 0) {
        m_bank.lend(loan);
        m_company.debt += loan;
        m_company.instalment = instalmentFor(m_company.debt);
        m_company.totals.borrowed += loan;
    }
    m_stats.shipyardPaid += price;
    m_company.totals.shipsBought += price;
    const EntityId ship = spawnCompanyShip(world, context.now, command.shipClass, port);
    world.components<OwnedShip>().add(ship, {price, insured, 0, 0});
    const Message kind = term(content::kShipClasses[command.shipClass].name);
    addJournal(context.now,
               loan > 0 ? msg("Compras {} ({}) por {} cr: {} de entrada y {} a crédito.", named(world, ship),
                              kind, number(price), number(command.downPayment), number(loan))
                        : msg("Compras {} ({}) por {} cr.", named(world, ship), kind, number(price)));
}

void Sandbox::onSellShipCommand(const SellShipCommand& command, const TickContext& context) {
    World& world = context.world;
    const auto reject = [&](const char* reason, const char* message) {
        ++m_stats.commandsRejected;
        GX_LOG_WARN("Sandbox", "sell ship command rejected: {}", reason);
        addJournal(context.now, msg(message));
    };
    const OwnedShip* owned =
        isPlayerShip(world, command.ship) ? world.components<OwnedShip>().tryGet(command.ship) : nullptr;
    if (owned == nullptr) {
        reject("not a ship of the company", GX_TEXT("Esa nave no se puede vender."));
        return;
    }
    if (command.ship == m_player) {
        reject("flagship", GX_TEXT("No puedes vender la nave que pilotas."));
        return;
    }
    const EntityId port = dockedPort(world, command.ship);
    const CelestialBody* body = world.components<CelestialBody>().tryGet(port);
    if (body == nullptr || body->kind != BodyKind::Station) {
        reject("not docked at a station",
               GX_TEXT("Para venderla, la nave tiene que estar atracada en una estación."));
        return;
    }
    const i64 price = std::llround(content::kShipResale * static_cast<f64>(owned->hullValue));
    addJournal(context.now,
               msg("Vendes {} a los astilleros por {} cr.", named(world, command.ship), number(price)));
    sellCompanyShip(world, command.ship, price, port, context.now);
}

void Sandbox::onFleetOrderCommand(const FleetOrderCommand& command, const TickContext& context) {
    World& world = context.world;
    const auto reject = [&](const char* reason, const char* message) {
        ++m_stats.commandsRejected;
        GX_LOG_WARN("Sandbox", "fleet order rejected: {}", reason);
        addJournal(context.now, msg(message));
    };
    FleetBrain* brain =
        isPlayerShip(world, command.ship) ? world.components<FleetBrain>().tryGet(command.ship) : nullptr;
    if (brain == nullptr) {
        reject("not a fleet ship", GX_TEXT("Esa nave no es de tu flota."));
        return;
    }
    const auto isPort = [&](EntityId entity) {
        return world.isAlive(entity) && world.components<Market>().contains(entity);
    };
    const CelestialBody* site =
        world.isAlive(command.site) ? world.components<CelestialBody>().tryGet(command.site) : nullptr;
    MiningControl* mining = world.components<MiningControl>().tryGet(command.ship);
    switch (command.order) {
    case FleetOrder::Hold:
        break;
    case FleetOrder::Escort:
        if (command.site.isValid() && (!isPlayerShip(world, command.site) || command.site == command.ship)) {
            reject("not a company ship", GX_TEXT("Solo puede escoltar a otra nave de tu compañía."));
            return;
        }
        break;
    case FleetOrder::Dock:
        if (!isPort(command.site)) {
            reject("not a port", GX_TEXT("Elige un puerto."));
            return;
        }
        break;
    case FleetOrder::Mine:
        if (mining == nullptr) {
            reject("no lasers", GX_TEXT("Esa nave no tiene láseres de minería."));
            return;
        }
        if (site == nullptr || !isAsteroidField(site->kind) ||
            !world.components<Deposit>().contains(command.site)) {
            reject("not a field", GX_TEXT("Elige un campo de asteroides."));
            return;
        }
        if (command.market.isValid() && !isPort(command.market)) {
            reject("not a market", GX_TEXT("Elige un puerto donde vender."));
            return;
        }
        break;
    case FleetOrder::Trade:
        if (world.components<CargoHold>().get(command.ship).capacity == 0) {
            reject("no hold", GX_TEXT("Esa nave no tiene bodega."));
            return;
        }
        break;
    default:
        reject("invalid order", GX_TEXT("Orden no válida."));
        return;
    }
    brain->order = command.order;
    brain->site =
        command.order != FleetOrder::Hold && command.order != FleetOrder::Trade ? command.site : EntityId{};
    brain->market = command.order == FleetOrder::Mine ? command.market : EntityId{};
    brain->task = FleetTask::Idle;
    brain->nextDecision = context.now;
    brain->target = 0;
    if (mining != nullptr) {
        mining->active = false;
    }
    if (CombatControl* orders = world.components<CombatControl>().tryGet(command.ship)) {
        orders->targetTrack = 0;
    }
    ShipControl& control = world.components<ShipControl>().get(command.ship);
    if (command.order == FleetOrder::Hold && !dockedPort(world, command.ship).isValid()) {
        resetOrders(control, FlightMode::Stop);
    }
    addJournal(context.now,
               msg("Nueva orden para {}: {}.", named(world, command.ship), orderText(world, *brain)));
}

void Sandbox::onFlagshipCommand(const FlagshipCommand& command, const TickContext& context) {
    World& world = context.world;
    const auto reject = [&](const char* reason, const char* message) {
        ++m_stats.commandsRejected;
        GX_LOG_WARN("Sandbox", "flagship command rejected: {}", reason);
        addJournal(context.now, msg(message));
    };
    if (!isPlayerShip(world, command.ship) || !world.components<FleetBrain>().contains(command.ship)) {
        reject("not a fleet ship", GX_TEXT("Esa nave no es de tu flota."));
        return;
    }
    const auto inHyperspace = [&](EntityId ship) {
        return world.isAlive(ship) &&
               world.components<ShipControl>().get(ship).phase == DrivePhase::Hyperspace;
    };
    if (inHyperspace(command.ship) || inHyperspace(m_player)) {
        reject("hyperspace", GX_TEXT("No se puede cambiar de nave en el hiperespacio."));
        return;
    }
    if (world.isAlive(m_player) && context.now < m_combatAlertUntil) {
        reject("in combat", GX_TEXT("No puedes cambiar de nave en pleno combate."));
        return;
    }
    const EntityId previous = world.isAlive(m_player) ? m_player : EntityId{};
    world.components<FleetBrain>().remove(command.ship);
    if (previous.isValid()) {
        // The ship left behind holds where it is under its captain.
        world.components<FleetBrain>().add(previous,
                                           {FleetOrder::Hold, {}, {}, FleetTask::Idle, context.now, 0, 0, 0});
        if (CombatControl* orders = world.components<CombatControl>().tryGet(previous)) {
            orders->targetTrack = 0;
        }
        if (MiningControl* mining = world.components<MiningControl>().tryGet(previous)) {
            mining->active = false;
        }
        ShipControl& control = world.components<ShipControl>().get(previous);
        if (!dockedPort(world, previous).isValid()) {
            resetOrders(control, FlightMode::Stop);
        }
    }
    m_player = command.ship;
    addJournal(context.now, msg("Tomas el mando de {}.", named(world, command.ship)));
    updateFlightRate(world, context.now);
}

void Sandbox::onLoanCommand(const LoanCommand& command, const TickContext& context) {
    World& world = context.world;
    const auto reject = [&](const char* reason, const char* message) {
        ++m_stats.commandsRejected;
        GX_LOG_WARN("Sandbox", "loan command rejected: {}", reason);
        addJournal(context.now, msg(message));
    };
    if (!m_config.finance) {
        reject("no bank", GX_TEXT("No hay banco en este sistema."));
        return;
    }
    if (command.borrow < 0 || command.repay < 0 || (command.borrow == 0 && command.repay == 0)) {
        reject("invalid amounts", GX_TEXT("Importe no válido."));
        return;
    }
    if (command.borrow > 0) {
        const i64 room = creditLimit(world) - m_company.debt;
        if (command.borrow > room) {
            reject("over the borrowing base", GX_TEXT("El banco no presta tanto contra tu flota."));
            return;
        }
        if (!m_bank.canLend(command.borrow, content::kBankReserveRatio)) {
            reject("bank short of cash", GX_TEXT("El banco no tiene fondos para prestarte ahora."));
            return;
        }
        m_bank.lend(command.borrow);
        m_company.debt += command.borrow;
        m_company.account.credits += command.borrow;
        m_company.totals.borrowed += command.borrow;
        m_company.instalment = instalmentFor(m_company.debt);
        addJournal(context.now, msg("El banco te presta {} cr. Debes {} cr.", number(command.borrow),
                                    number(m_company.debt)));
    }
    if (command.repay > 0) {
        const i64 amount = std::min(command.repay, m_company.debt);
        if (amount <= 0 || amount > m_company.account.credits) {
            reject("cannot repay", GX_TEXT("No tienes créditos para pagar eso."));
            return;
        }
        m_company.account.credits -= amount;
        m_bank.repay(amount);
        m_company.debt -= amount;
        m_company.totals.repaid += amount;
        m_company.instalment = instalmentFor(m_company.debt);
        addJournal(context.now,
                   m_company.debt > 0
                       ? msg("Devuelves {} cr al banco. Debes {} cr.", number(amount), number(m_company.debt))
                       : msg("Devuelves {} cr al banco: tu deuda está saldada.", number(amount)));
    }
}

void Sandbox::onInsureCommand(const InsureCommand& command, const TickContext& context) {
    World& world = context.world;
    const auto reject = [&](const char* reason, const char* message) {
        ++m_stats.commandsRejected;
        GX_LOG_WARN("Sandbox", "insure command rejected: {}", reason);
        addJournal(context.now, msg(message));
    };
    OwnedShip* owned =
        isPlayerShip(world, command.ship) ? world.components<OwnedShip>().tryGet(command.ship) : nullptr;
    if (owned == nullptr) {
        reject("not a ship of the company", GX_TEXT("Esa nave no se puede asegurar."));
        return;
    }
    if (!m_config.finance) {
        reject("no mutual", GX_TEXT("No hay mutua de seguros en este sistema."));
        return;
    }
    if (!command.insured && m_company.debt > 0) {
        reject("debt", GX_TEXT("El banco exige asegurar los cascos mientras le debas dinero."));
        return;
    }
    if (owned->insured == command.insured) {
        return;
    }
    owned->insured = command.insured;
    addJournal(context.now, command.insured ? msg("{} queda asegurada con la Mutua de Fletadores.",
                                                  named(world, command.ship))
                                            : msg("Cancelas el seguro de {}.", named(world, command.ship)));
}

void Sandbox::onMineCommand(const MineCommand& command, const TickContext& context) {
    World& world = context.world;
    const auto reject = [&](const char* reason, const char* message) {
        ++m_stats.commandsRejected;
        GX_LOG_WARN("Sandbox", "mine command rejected: {}", reason);
        addJournal(context.now, msg(message));
    };
    MiningControl* mining = command.ship == m_player && world.isAlive(m_player)
                                ? world.components<MiningControl>().tryGet(m_player)
                                : nullptr;
    if (mining == nullptr) {
        reject("no lasers", GX_TEXT("Tu nave no tiene láseres de minería."));
        return;
    }
    if (!command.on) {
        if (mining->active) {
            mining->active = false;
            addJournal(context.now, msg("Minería detenida."));
        }
        return;
    }
    const CelestialBody* body =
        world.isAlive(command.field) ? world.components<CelestialBody>().tryGet(command.field) : nullptr;
    if (body == nullptr || !isAsteroidField(body->kind) ||
        !world.components<Deposit>().contains(command.field)) {
        reject("not a field", GX_TEXT("Eso no es un campo de asteroides."));
        return;
    }
    const Kinematics& ship = world.components<Kinematics>().get(m_player);
    const OrbitState field = bodyStateAt(world, command.field, context.now);
    if (length(ship.position - field.position) > content::kMiningRange ||
        length(ship.velocity - field.velocity) > content::kMiningSpeed) {
        reject("out of the field", GX_TEXT("Entra en el campo y detén la nave para minar."));
        return;
    }
    if (world.components<CargoHold>().get(m_player).space() == 0) {
        reject("hold full", GX_TEXT("La bodega está llena."));
        return;
    }
    startMining(world, m_player, command.field);
    addJournal(context.now, msg("Láseres de minería en marcha en {}.", named(world, command.field)));
}

// --- Mining
// -------------------------------------------------------------------------------------------------

void Sandbox::startMining(World& world, EntityId ship, EntityId field) {
    MiningControl& mining = world.components<MiningControl>().get(ship);
    if (mining.field != field) {
        mining.progress = 0.0;
    }
    mining.field = field;
    mining.active = true;
}

void Sandbox::updateMining(const TickContext& context) {
    World& world = context.world;
    const f64 hours = context.dt.toSeconds() / 3600.0;
    ComponentStore<MiningControl>& store = world.components<MiningControl>();
    ComponentStore<Deposit>& deposits = world.components<Deposit>();
    for (usize i = 0; i < store.size(); ++i) {
        MiningControl& mining = store.values()[i];
        if (!mining.active) {
            continue;
        }
        const EntityId ship = store.entities()[i];
        const auto stop = [&](Message reason) {
            mining.active = false;
            if (ship == m_player) {
                addJournal(context.now, std::move(reason));
            }
        };
        Deposit* deposit = world.isAlive(mining.field) ? deposits.tryGet(mining.field) : nullptr;
        const ShipModules* modules = world.components<ShipModules>().tryGet(ship);
        CargoHold* hold = world.components<CargoHold>().tryGet(ship);
        if (deposit == nullptr || modules == nullptr || hold == nullptr) {
            stop(msg("Minería detenida."));
            continue;
        }
        const f64 rate = miningRate(*modules);
        if (rate <= 0.0) {
            stop(msg("Los láseres de minería no funcionan: minería detenida."));
            continue;
        }
        if (hold->space() == 0) {
            stop(msg("Bodega llena: minería detenida."));
            continue;
        }
        // The lasers hold only among the rocks and with the ship's motion matched to the field's.
        const Kinematics& kinematics = world.components<Kinematics>().get(ship);
        const OrbitState field = bodyStateAt(world, mining.field, context.now);
        if (length(kinematics.position - field.position) > content::kMiningRange ||
            length(kinematics.velocity - field.velocity) > content::kMiningSpeed) {
            stop(msg("Fuera del campo: minería detenida."));
            continue;
        }
        mining.progress += rate * hours;
        const u32 whole = std::min(static_cast<u32>(mining.progress), hold->space());
        if (whole == 0) {
            continue;
        }
        const u32 taken = extract(*deposit, whole);
        hold->add(deposit->good, taken);
        // A dry field leaves the lasers idle until rocks drift in: nothing is banked meanwhile.
        mining.progress = taken < whole ? 0.0 : mining.progress - whole;
        if (isPlayerShip(world, ship)) {
            m_company.totals.tonnesMined += taken;
        }
    }
}

// --- The fleet's captains -------------------------------------------------------------------------------

void Sandbox::sendTo(World& world, EntityId ship, FleetBrain& brain, EntityId target) {
    ShipControl& control = world.components<ShipControl>().get(ship);
    resetOrders(control, FlightMode::Approach);
    control.target = target;
    control.standoff = standoffDistance(world, target);
    brain.task = FleetTask::Travelling;
}

EntityId Sandbox::bestMarket(const World& world, EntityId ship, GoodId good, f64 tonnes, SimTime now,
                             EntityId except, EntityId returnTo, f64 workSeconds) const {
    // Value per second of the whole cycle (renewal-reward: the long-run earnings rate is what a cycle earns
    // over how long it lasts), so a miner does not dump ore at the nearest port that takes it when a better
    // one is a little farther and the lasers need an hour anyway. Discounted by recent losses, with what the
    // company's other ships are already carrying there. Stale knowledge is shrunk towards the market at its
    // target stock (the base price) rather than towards nothing: a seller that only trusted fresh prices
    // would keep selling where it last sold, however low its own sales had pushed the price there.
    const Vec3d position = world.components<Kinematics>().get(ship).position;
    const u32 shipClass = world.components<ShipIdentity>().get(ship).shipClass;
    const f64 speed = content::kShipClasses[shipClass].hyperspaceSpeed;
    const bool back = returnTo.isValid() && world.isAlive(returnTo);
    const Vec3d home = back ? bodyStateAt(world, returnTo, now).position : Vec3d{};
    EntityId best;
    f64 bestScore = 0.0;
    for (const PortPrices& known : m_playerPrices.ports) {
        if (known.port == except || !world.isAlive(known.port)) {
            continue;
        }
        if (known.find(good) == nullptr) {
            continue; // not traded there
        }
        const f64 freshness = std::exp2(-(now - known.observed).toSeconds() / content::kKnowledgeHalfLife);
        const f64 atTarget = tonnes * m_economy.basePrice(good) * (1.0 - kMarketSpread);
        const f64 value =
            freshness * saleValue(known, good, tonnes, m_fleetInflight, false) + (1.0 - freshness) * atTarget;
        if (value <= 0.0) {
            continue;
        }
        const Vec3d there = bodyStateAt(world, known.port, now).position;
        f64 seconds = workSeconds + content::kTripOverhead + length(there - position) / speed;
        if (back) {
            seconds += content::kTripOverhead + length(home - there) / speed;
        }
        const f64 score = value / (seconds * (1.0 + danger(known.port, now)));
        if (score > bestScore) {
            bestScore = score;
            best = known.port;
        }
    }
    return best;
}

void Sandbox::sellFleetCargo(World& world, EntityId ship, EntityId port, SimTime now) {
    Market* market = world.components<Market>().tryGet(port);
    CargoHold* hold = world.components<CargoHold>().tryGet(ship);
    if (market == nullptr || hold == nullptr) {
        return;
    }
    // Every visit teaches the company the port's prices, and stations hand out the traders' bulletin.
    m_playerPrices.observe(port, *market, m_economy.goods(), now);
    const CelestialBody* body = world.components<CelestialBody>().tryGet(port);
    if (body != nullptr && body->kind == BodyKind::Station && !hostile()) {
        m_playerPrices.mergeNewer(m_traderPrices);
    }
    if (hold->used() == 0) {
        return;
    }
    if (hostile()) {
        addJournal(
            now,
            msg("{} no puede vender en {}: tu reputación es hostil.", named(world, ship), named(world, port)),
            JournalKind::Fleet);
        return;
    }
    OwnedShip* owned = world.components<OwnedShip>().tryGet(ship);
    const std::vector<CargoItem> items = hold->items; // selling edits the hold
    for (const CargoItem& item : items) {
        MarketGood* good = market->find(item.good);
        if (good == nullptr) {
            continue;
        }
        const TradeResult sold = sellGoods(*good, m_economy.basePrice(item.good), item.tonnes, *hold,
                                           m_company.account, content::kTradeTaxRate);
        if (sold.tonnes == 0) {
            continue;
        }
        collectTax(sold.tax);
        m_company.totals.sales += sold.credits;
        m_company.totals.taxes += sold.tax;
        if (owned != nullptr) {
            owned->income += sold.credits - sold.tax;
        }
        addJournal(now,
                   msg("{} vende {} t de {} en {} por {} cr.", named(world, ship), number(sold.tonnes),
                       term(m_economy.goods()[item.good].name), named(world, port),
                       number(sold.credits - sold.tax)),
                   JournalKind::Fleet);
    }
    m_playerPrices.observe(port, *market, m_economy.goods(), now);
}

void Sandbox::fleetMine(World& world, EntityId ship, FleetBrain& brain, SimTime now) {
    const Deposit* deposit =
        world.isAlive(brain.site) ? world.components<Deposit>().tryGet(brain.site) : nullptr;
    MiningControl* mining = world.components<MiningControl>().tryGet(ship);
    if (deposit == nullptr || mining == nullptr) {
        brain.order = FleetOrder::Hold;
        brain.task = FleetTask::Idle;
        return;
    }
    CargoHold& hold = world.components<CargoHold>().get(ship);
    const ShipControl& control = world.components<ShipControl>().get(ship);
    const EntityId docked = dockedPort(world, ship);
    const auto goSell = [&](EntityId except) {
        mining->active = false;
        const GoodId good = hold.items.empty() ? deposit->good : hold.items.front().good;
        EntityId market = brain.market.isValid() && brain.market != except ? brain.market : EntityId{};
        if (!market.isValid()) {
            const f64 rate = miningRate(world.components<ShipModules>().get(ship));
            const f64 refill = rate > 0.0 ? static_cast<f64>(hold.capacity) / rate * 3600.0 : 0.0;
            market = bestMarket(world, ship, good, static_cast<f64>(hold.amount(good)), now, except,
                                brain.site, refill);
        }
        if (market.isValid()) {
            sendTo(world, ship, brain, market);
            ++brain.trips;
        } else {
            brain.task = FleetTask::Idle; // nobody known buys it: wait for news
        }
    };

    switch (brain.task) {
    case FleetTask::Travelling:
        if (!control.arrived) {
            return;
        }
        if (docked.isValid() && control.target == docked) {
            sellFleetCargo(world, ship, docked, now);
            brain.task = FleetTask::Idle;
            // Most of the load unsold (that market is full): try another before going back.
            if (hold.used() * 2 > hold.capacity) {
                goSell(docked);
                return;
            }
        } else if (control.target == brain.site) {
            startMining(world, ship, brain.site);
            brain.task = FleetTask::Mining;
            return;
        } else {
            brain.task = FleetTask::Idle;
        }
        break;
    case FleetTask::Mining:
        if (hold.space() == 0) {
            goSell({});
            return;
        }
        if (deposit->reserve < 1.0) {
            // Dry: another field of the same kind with rock to spare, if the company's charts show one.
            EntityId alternative;
            f64 reserve = kFieldSwitchReserve * deposit->size;
            for (const EntityId field : m_fields) {
                const Deposit* other = world.components<Deposit>().tryGet(field);
                if (field != brain.site && other != nullptr && other->good == deposit->good &&
                    other->reserve > reserve) {
                    alternative = field;
                    reserve = other->reserve;
                }
            }
            if (alternative.isValid()) {
                addJournal(now,
                           msg("{} deja {}, agotado, por {}.", named(world, ship), named(world, brain.site),
                               named(world, alternative)),
                           JournalKind::Fleet);
                brain.site = alternative;
                mining->active = false;
                sendTo(world, ship, brain, alternative);
                return;
            }
            if (hold.used() * 2 >= hold.capacity) {
                goSell({}); // half a load: sell it while the field recovers
                return;
            }
        }
        if (!mining->active) {
            // Stopped (drifted out of the field): back among the rocks.
            sendTo(world, ship, brain, brain.site);
        }
        return;
    default:
        break;
    }
    if (brain.task == FleetTask::Idle) {
        if (hold.space() == 0) {
            goSell({});
        } else {
            sendTo(world, ship, brain, brain.site);
        }
    }
}

void Sandbox::fleetTrade(World& world, EntityId ship, FleetBrain& brain, SimTime now, u64 worldSeed) {
    const ShipControl& control = world.components<ShipControl>().get(ship);
    const EntityId docked = dockedPort(world, ship);
    if (brain.task == FleetTask::Travelling) {
        if (!docked.isValid() || control.target != docked) {
            if (control.mode != FlightMode::Approach) {
                brain.task = FleetTask::Idle; // lost its way (a stop order, a vanished target)
            }
            return;
        }
        sellFleetCargo(world, ship, docked, now);
        brain.task = FleetTask::Idle;
        brain.nextDecision = now + kFleetDwell;
        return;
    }
    if (!docked.isValid()) {
        // In space (a new order): the nearest port first.
        const Vec3d position = world.components<Kinematics>().get(ship).position;
        EntityId nearest;
        f64 distance = 0.0;
        for (const EntityId port : m_ports) {
            const f64 d = length(bodyStateAt(world, port, now).position - position);
            if (!nearest.isValid() || d < distance) {
                nearest = port;
                distance = d;
            }
        }
        if (nearest.isValid()) {
            sendTo(world, ship, brain, nearest);
        }
        return;
    }
    if (now < brain.nextDecision || m_ports.size() < 2) {
        return;
    }
    Market& market = world.components<Market>().get(docked);
    m_playerPrices.observe(docked, market, m_economy.goods(), now);
    CargoHold& hold = world.components<CargoHold>().get(ship);
    if (hold.used() > 0) {
        sellFleetCargo(world, ship, docked, now);
    }
    // It trades with the company's knowledge and money, leaving a reserve in the account.
    const i64 budget = std::max<i64>(0, m_company.account.credits - content::kFleetCashReserve);
    Rng rng = Rng::forStream(worldSeed, hashCombine(kFleetTradeStream, entityKey(ship)), brain.trips);
    const TripPlan plan = planTrip(world, ship, m_playerPrices, m_fleetInflight, budget, false, now, rng);
    if (plan.tonnes > 0) {
        const TradeResult bought = buyGoods(*market.find(plan.load), m_economy.basePrice(plan.load),
                                            plan.tonnes, hold, m_company.account, content::kTradeTaxRate);
        collectTax(bought.tax);
        m_company.totals.purchases += bought.credits;
        m_company.totals.taxes += bought.tax;
        if (OwnedShip* owned = world.components<OwnedShip>().tryGet(ship)) {
            owned->expenses += bought.credits + bought.tax;
        }
        m_playerPrices.observe(docked, market, m_economy.goods(), now);
    }
    for (const CargoItem& item : hold.items) {
        bool found = false;
        for (Delivery& delivery : m_fleetInflight) {
            if (delivery.port == plan.destination && delivery.good == item.good) {
                delivery.tonnes += item.tonnes;
                found = true;
            }
        }
        if (!found) {
            m_fleetInflight.push_back({plan.destination, item.good, static_cast<f64>(item.tonnes)});
        }
    }
    sendTo(world, ship, brain, plan.destination);
    ++brain.trips;
}

void Sandbox::fleetEscort(World& world, EntityId ship, FleetBrain& brain, SimTime now) {
    ShipControl& control = world.components<ShipControl>().get(ship);
    CombatControl* orders = world.components<CombatControl>().tryGet(ship);
    SensorSuite& suite = world.components<SensorSuite>().get(ship);
    const auto standDown = [&] {
        if (orders != nullptr) {
            orders->targetTrack = 0;
        }
        suite.activeOn = false;
        brain.target = 0;
    };
    // The ship it guards: the one named in its order, else the one the player flies.
    const EntityId ward =
        brain.site.isValid() && isPlayerShip(world, brain.site) && brain.site != ship ? brain.site : m_player;
    if (!world.isAlive(ward)) {
        // Nobody to escort (the player is waiting for a new ship): hold.
        if (brain.task != FleetTask::Idle) {
            standDown();
            resetOrders(control, FlightMode::Stop);
            brain.task = FleetTask::Idle;
        }
        return;
    }
    const ShipModules& modules = world.components<ShipModules>().get(ship);
    const ShipModule* structure = structureOf(modules);
    const bool armed = orders != nullptr && moduleEfficiency(modules, ModuleType::Weapon) > 0.0;
    if (orders != nullptr &&
        ((structure != nullptr && structure->fraction() < content::kFleetRetreatStructure) || !armed)) {
        // Beaten: to the nearest station's yards, then back to the flagship.
        standDown();
        const EntityId station =
            nearestStation(world, world.components<Kinematics>().get(ship).position, now);
        if (station.isValid()) {
            sendTo(world, ship, brain, station);
            brain.task = FleetTask::Fleeing;
            addJournal(now, msg("{} se retira a reparar a {}.", named(world, ship), named(world, station)));
        }
        return;
    }

    // Threats, from the company's sensor picture: the flagship's own target, else the nearest raider near
    // the ship it guards.
    const Vec3d flagship = world.components<Kinematics>().get(ward).position;
    const FactionPicture& picture = m_sensors.picture(content::kFactionPlayer);
    const auto estimate = [&](const SensorContact& contact) {
        return contact.position + contact.velocity * (now - contact.lastSeen).toSeconds();
    };
    const auto raider = [](const SensorContact& contact) {
        return (contact.level == ContactLevel::Identified && contact.faction == content::kFactionPirates) ||
               (contact.level == ContactLevel::Classified && contact.shipClass == content::kShipClassRaider);
    };
    const auto fightable = [&](const SensorContact& contact) {
        return length(contact.velocity) < content::kHyperspaceSpeedThreshold &&
               length(estimate(contact) - flagship) < content::kEscortLeash;
    };
    u32 threat = 0;
    if (armed) {
        const CombatControl* flagshipOrders =
            ward == m_player ? world.components<CombatControl>().tryGet(m_player) : nullptr;
        const SensorContact* chosen =
            flagshipOrders != nullptr && flagshipOrders->targetTrack != 0
                ? m_sensors.findContact(content::kFactionPlayer, flagshipOrders->targetTrack)
                : nullptr;
        if (chosen == nullptr && brain.task == FleetTask::Engaging) {
            chosen = m_sensors.findContact(content::kFactionPlayer, brain.target); // keep at it
        }
        if (chosen != nullptr && !fightable(*chosen)) {
            chosen = nullptr;
        }
        if (chosen == nullptr) {
            f64 nearest = content::kEscortEngageRange;
            for (const SensorContact& contact : picture.contacts) {
                const f64 distance = length(estimate(contact) - flagship);
                if (raider(contact) && fightable(contact) && distance < nearest) {
                    chosen = &contact;
                    nearest = distance;
                }
            }
        }
        threat = chosen != nullptr ? chosen->trackId : 0;
    }
    if (threat != 0) {
        if (brain.task != FleetTask::Engaging || brain.target != threat) {
            resetOrders(control, FlightMode::Pursue);
            control.track = threat;
            control.standoff = content::kPatrolStandoff;
            orders->targetTrack = threat;
            suite.activeOn = suite.activeStrength > 0.0; // fire control
            brain.task = FleetTask::Engaging;
            brain.target = threat;
            addJournal(now, msg("{} ataca al contacto {}.", named(world, ship), number(threat)),
                       JournalKind::Fleet);
        }
        return;
    }
    if (brain.task == FleetTask::Engaging) {
        standDown();
    }
    if (control.mode != FlightMode::Approach || control.target != ward) {
        resetOrders(control, FlightMode::Approach);
        control.target = ward;
        control.standoff = content::kEscortStandoff;
    }
    brain.task = FleetTask::Escorting;
}

void Sandbox::updateFleet(const TickContext& context) {
    World& world = context.world;
    const SimTime now = context.now;
    ComponentStore<FleetBrain>& brains = world.components<FleetBrain>();
    m_fleetInflight.clear();
    if (brains.size() == 0) {
        return;
    }
    // The company's cargo already on its way, so its traders do not all fly the same load to the same port.
    for (const EntityId ship : brains.entities()) {
        const ShipControl& control = world.components<ShipControl>().get(ship);
        const CargoHold* hold = world.components<CargoHold>().tryGet(ship);
        if (hold == nullptr || control.mode != FlightMode::Approach || control.arrived ||
            !world.components<Market>().contains(control.target)) {
            continue;
        }
        for (const CargoItem& item : hold->items) {
            m_fleetInflight.push_back({control.target, item.good, static_cast<f64>(item.tonnes)});
        }
    }
    const ComponentStore<CelestialBody>& bodies = world.components<CelestialBody>();
    for (usize i = 0; i < brains.size(); ++i) {
        const EntityId ship = brains.entities()[i];
        FleetBrain& brain = brains.values()[i];
        const ShipControl& control = world.components<ShipControl>().get(ship);
        const ShipModules& modules = world.components<ShipModules>().get(ship);
        if (!hasPower(modules) || control.phase == DrivePhase::Hyperspace) {
            continue; // disabled and drifting, or nothing to decide until it drops out
        }
        if (brain.task == FleetTask::Fleeing) {
            // Hide at the station until the yards have patched the hull, then back to work.
            const CelestialBody* port = bodies.tryGet(dockedPort(world, ship));
            if (port == nullptr || port->kind != BodyKind::Station) {
                const CelestialBody* heading = bodies.tryGet(control.target);
                if (control.mode != FlightMode::Approach || heading == nullptr ||
                    heading->kind != BodyKind::Station) {
                    const EntityId station =
                        nearestStation(world, world.components<Kinematics>().get(ship).position, now);
                    if (station.isValid()) {
                        sendTo(world, ship, brain, station);
                        brain.task = FleetTask::Fleeing;
                    }
                }
                continue;
            }
            const ShipModule* structure = structureOf(modules);
            if (structure != nullptr && structure->fraction() < content::kFleetRepairedStructure) {
                continue;
            }
            brain.task = FleetTask::Idle;
            brain.nextDecision = now;
        }
        switch (brain.order) {
        case FleetOrder::Hold:
        case FleetOrder::Count:
            break;
        case FleetOrder::Dock:
            if (!world.isAlive(brain.site)) {
                brain.order = FleetOrder::Hold;
            } else if (control.mode != FlightMode::Approach || control.target != brain.site) {
                sendTo(world, ship, brain, brain.site);
            } else if (control.arrived) {
                brain.task = FleetTask::Idle;
            }
            break;
        case FleetOrder::Mine:
            fleetMine(world, ship, brain, now);
            break;
        case FleetOrder::Trade:
            fleetTrade(world, ship, brain, now, context.worldSeed);
            break;
        case FleetOrder::Escort:
            fleetEscort(world, ship, brain, now);
            break;
        }
    }
}

} // namespace gx
