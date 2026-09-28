#include "Tests/TestFramework.h"

#include "Engine/Jobs/JobSystem.h"
#include "Engine/Text/Localization.h"
#include "Game/Sandbox/Content.h"
#include "Game/Sandbox/Sandbox.h"
#include "Simulation/Economy/Economy.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Bodies/CelestialBody.h"
#include "Space/Generation/StarSystemGenerator.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

using namespace gx;

namespace {

SandboxConfig peaceful() {
    SandboxConfig config;
    config.pirates = 0;
    return config;
}

// A game where the player runs a company (ADR-037), with helpers to buy ships and give them orders.
struct Company {
    explicit Company(u32 workers, const SandboxConfig& config = peaceful())
        : jobs(workers), sandbox(config), simulation({.seed = config.seed}, jobs) {
        sandbox.install(simulation);
        sandbox.populate(simulation);
    }
    JobSystem jobs;
    Sandbox sandbox;
    Simulation simulation;

    [[nodiscard]] World& world() { return simulation.world(); }
    // Credits given by the test: the account identity starts from here.
    void give(i64 credits) {
        sandbox.account().credits += credits;
        injected += credits;
    }
    // Buys at the station the player is docked at; returns the new ship (invalid if refused).
    EntityId buy(u32 shipClass, i64 downPayment, bool insured = true) {
        const usize before = world().components<FleetBrain>().size();
        simulation.submitCommand(BuyShipCommand{sandbox.playerShip(), shipClass, downPayment, insured});
        simulation.runFor(SimDuration::seconds(1));
        const auto& brains = world().components<FleetBrain>();
        return brains.size() > before ? brains.entities().back() : EntityId{};
    }
    void pilot(EntityId ship, FlightMode mode, EntityId target = {}, Vec3d point = {}) {
        simulation.submitCommand(PilotCommand{ship, mode, target, point, {}});
    }
    void order(EntityId ship, FleetOrder order, EntityId site = {}, EntityId market = {}) {
        simulation.submitCommand(FleetOrderCommand{ship, order, site, market});
        simulation.runFor(SimDuration::seconds(1));
    }
    // The rock field nearest to the player's home station.
    [[nodiscard]] EntityId rockField() {
        const Vec3d home = bodyStateAt(world(), sandbox.homePort(), simulation.now()).position;
        EntityId best;
        f64 bestDistance = 0.0;
        for (const EntityId field : sandbox.fields()) {
            const f64 d = length(bodyStateAt(world(), field, simulation.now()).position - home);
            if (world().components<CelestialBody>().get(field).kind == BodyKind::AsteroidField &&
                (!best.isValid() || d < bestDistance)) {
                best = field;
                bestDistance = d;
            }
        }
        return best;
    }
    [[nodiscard]] bool journalContains(const std::string& text) const {
        for (const JournalEntry& entry : sandbox.journal()) {
            if (render(entry.text, nullptr).find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    // Every movement of the account is booked in the company's totals (CompanyTotals).
    void expectAccountAddsUp() const {
        const CompanyTotals& t = sandbox.company().totals;
        const i64 expected = content::kPlayerStartCredits + injected + t.sales - t.purchases - t.taxes +
                             t.contracts + t.bounties - t.repairs - t.wages - t.premiums - t.interest +
                             t.claims - t.shipsBought + t.shipsSold + t.borrowed - t.repaid;
        GX_EXPECT_EQ(sandbox.company().account.credits, expected);
        GX_EXPECT_EQ(sandbox.company().debt, t.borrowed - t.repaid);
    }
    void expectBankBalances() {
        i64 debts = sandbox.company().debt;
        for (const TraderFinance& books : world().components<TraderFinance>().values()) {
            debts += books.debt;
        }
        GX_EXPECT(sandbox.bank().balanced());
        GX_EXPECT(sandbox.mutual().balanced());
        GX_EXPECT_EQ(debts, sandbox.bank().loans);
    }

    i64 injected = 0;
};

Catalog loadEnglish() {
    std::ifstream in(std::filesystem::path(GX_SOURCE_DATA_DIR) / "lang" / "en.po", std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Catalog catalog;
    std::string error;
    GX_CHECK(catalog.loadPo(text, error), "en.po: {}", error);
    return catalog;
}

} // namespace

GX_TEST(Fleet, GeneratedSystemsHaveAnAsteroidBelt) {
    for (const u64 seed : {1ull, 2ull, 3ull, 42ull, 2400ull, 99999ull}) {
        const StarSystemDesc system = generateStarSystem(seed);
        std::vector<f64> planets;
        std::vector<f64> fields;
        u32 rock = 0;
        u32 ice = 0;
        for (const BodyDesc& body : system.bodies) {
            if (isPlanet(body.kind)) {
                planets.push_back(body.orbit.semiMajorAxis);
            } else if (isAsteroidField(body.kind)) {
                GX_EXPECT_EQ(body.parent, 0); // around the star
                GX_EXPECT_EQ(body.gm, 0.0);   // no gravity well
                fields.push_back(body.orbit.semiMajorAxis);
                rock += body.kind == BodyKind::AsteroidField ? 1 : 0;
                ice += body.kind == BodyKind::IceField ? 1 : 0;
            }
        }
        GX_EXPECT_EQ(rock, 2u);
        GX_EXPECT_EQ(ice, 1u);
        // Clear of every planet's orbit (and so of their gravity wells).
        for (const f64 field : fields) {
            for (const f64 planet : planets) {
                GX_EXPECT(std::max(field, planet) / std::min(field, planet) > 1.1);
            }
        }
    }
}

GX_TEST(Fleet, DepositsRunDryAndRecover) {
    Deposit deposit{content::kGoodOre, 10.5, 100.0, 60.0, 0.0, 0.0};
    GX_EXPECT_EQ(extract(deposit, 4), 4u);
    GX_EXPECT_EQ(extract(deposit, 10), 6u); // only whole tonnes
    GX_EXPECT_EQ(extract(deposit, 1), 0u);
    GX_EXPECT_NEAR(deposit.reserve, 0.5, 1e-12);
    recover(deposit, 0.5);
    GX_EXPECT_NEAR(deposit.reserve, 30.5, 1e-12);
    recover(deposit, 10.0);
    GX_EXPECT_NEAR(deposit.reserve, 100.0, 1e-12); // never above its size
    GX_EXPECT_NEAR(deposit.extracted, 10.0, 1e-12);
    GX_EXPECT_NEAR(deposit.recovered, 99.5, 1e-12);
}

GX_TEST(Fleet, TheSandboxHasFieldsToMine) {
    Company company(0);
    const World& world = company.world();
    GX_REQUIRE(company.sandbox.fields().size() == 3u);
    for (const EntityId field : company.sandbox.fields()) {
        const Deposit* deposit = world.components<Deposit>().tryGet(field);
        GX_REQUIRE(deposit != nullptr);
        const BodyKind kind = world.components<CelestialBody>().get(field).kind;
        GX_EXPECT_EQ(deposit->good, static_cast<GoodId>(kind == BodyKind::IceField ? content::kGoodWater
                                                                                   : content::kGoodOre));
        GX_EXPECT_EQ(deposit->reserve, content::kFieldDeposit);
        GX_EXPECT(!world.components<Market>().contains(field)); // not a port
    }
    GX_EXPECT_EQ(company.sandbox.company().account.credits, content::kPlayerStartCredits);
    GX_EXPECT(
        !company.world().components<Wallet>().contains(company.sandbox.playerShip())); // the account pays
}

GX_TEST(Fleet, ShipsAreBoughtAtAStationWithTheBanksHelp) {
    Company company(0);
    World& world = company.world();
    const i64 loansBefore = company.sandbox.bank().loans;
    // 2,000 cr: a quarter of a Minero down, the bank lends the rest (and requires insurance).
    const i64 price = content::kShipPrices[content::kShipClassMiner];
    const i64 down = price / 4;
    const EntityId miner = company.buy(content::kShipClassMiner, down, false);
    GX_REQUIRE(miner.isValid());
    const OwnedShip& owned = world.components<OwnedShip>().get(miner);
    GX_EXPECT_EQ(owned.hullValue, price);
    GX_EXPECT(owned.insured);
    GX_EXPECT_EQ(world.components<ShipIdentity>().get(miner).faction,
                 static_cast<u32>(content::kFactionPlayer));
    GX_EXPECT(world.components<MiningControl>().contains(miner));
    GX_EXPECT_EQ(company.sandbox.company().debt, price - down);
    GX_EXPECT_EQ(company.sandbox.bank().loans, loansBefore + price - down);
    GX_EXPECT_EQ(company.sandbox.company().account.credits, content::kPlayerStartCredits - down);
    GX_EXPECT_EQ(company.sandbox.creditLimit(world), static_cast<i64>(0.75 * static_cast<f64>(price)));
    GX_EXPECT(company.journalContains("a crédito"));

    // More than the hulls can secure: refused.
    const u64 rejected = company.sandbox.stats().commandsRejected;
    GX_EXPECT(!company.buy(content::kShipClassEscort, 0).isValid());
    GX_EXPECT_EQ(company.sandbox.stats().commandsRejected, rejected + 1);
    company.expectAccountAddsUp();
    company.expectBankBalances();

    // Away from the station's yards: refused.
    company.give(20'000);
    company.pilot(company.sandbox.playerShip(), FlightMode::MoveTo, {},
                  world.components<Kinematics>().get(company.sandbox.playerShip()).position +
                      Vec3d{1e8, 0.0, 0.0});
    company.simulation.runFor(SimDuration::seconds(5));
    GX_EXPECT(
        !company.buy(content::kShipClassHauler, content::kShipPrices[content::kShipClassHauler]).isValid());
    GX_EXPECT(company.journalContains("atraca en una"));
}

GX_TEST(Fleet, MinersCutOreAndSellItWhereItPays) {
    Company company(3);
    company.give(20'000);
    const EntityId miner =
        company.buy(content::kShipClassMiner, content::kShipPrices[content::kShipClassMiner]);
    GX_REQUIRE(miner.isValid());
    const EntityId field = company.rockField();
    company.order(miner, FleetOrder::Mine, field);
    for (int step = 0; step < 8; ++step) {
        company.simulation.runFor(SimDuration::minutes(30));
        company.expectAccountAddsUp();
        company.expectBankBalances();
    }
    const CompanyTotals& totals = company.sandbox.company().totals;
    const World& world = company.world();
    GX_EXPECT(totals.tonnesMined >= content::kCargoCapacity[content::kShipClassMiner]);
    GX_EXPECT(totals.sales > 0);
    GX_EXPECT(world.components<OwnedShip>().get(miner).income > 0);
    f64 extracted = 0.0;
    for (const Deposit& deposit : world.components<Deposit>().values()) {
        extracted += deposit.extracted;
    }
    GX_EXPECT_NEAR(extracted, static_cast<f64>(totals.tonnesMined), 1e-9); // nobody else mines
    GX_EXPECT(company.journalContains("t de Mineral"));
}

GX_TEST(Fleet, ThePlayerTakesCommandAndMinesByHand) {
    Company company(0);
    company.give(20'000);
    World& world = company.world();
    const EntityId courier = company.sandbox.playerShip();
    const EntityId miner =
        company.buy(content::kShipClassMiner, content::kShipPrices[content::kShipClassMiner]);
    GX_REQUIRE(miner.isValid());
    company.simulation.submitCommand(FlagshipCommand{miner});
    company.simulation.runFor(SimDuration::seconds(1));
    GX_REQUIRE(company.sandbox.playerShip() == miner);
    GX_EXPECT(!world.components<FleetBrain>().contains(miner));
    GX_EXPECT(world.components<FleetBrain>().contains(courier)); // it joins the fleet, holding
    const u64 rejected = company.sandbox.stats().commandsRejected;
    company.pilot(courier, FlightMode::Stop);
    company.simulation.runFor(SimDuration::seconds(1));
    GX_EXPECT_EQ(company.sandbox.stats().commandsRejected, rejected + 1); // it takes standing orders now

    // To the field (placed at its edge to keep the test short), then the lasers.
    const EntityId field = company.rockField();
    const OrbitState at = bodyStateAt(world, field, company.simulation.now());
    world.components<Kinematics>().get(miner) = {at.position + Vec3d{30'000.0, 0.0, 0.0}, at.velocity, {}};
    company.pilot(miner, FlightMode::Approach, field);
    company.simulation.runFor(SimDuration::minutes(2));
    GX_REQUIRE(world.components<ShipControl>().get(miner).arrived);
    company.simulation.submitCommand(MineCommand{miner, field, true});
    company.simulation.runFor(SimDuration::minutes(30));
    const u32 ore = world.components<CargoHold>().get(miner).amount(content::kGoodOre);
    const f64 expected = 2.0 * content::kMiningRatePerModule * 0.5; // two lasers, half an hour
    GX_EXPECT(ore >= static_cast<u32>(expected) - 2 && ore <= static_cast<u32>(expected));
    GX_EXPECT(world.components<MiningControl>().get(miner).active);

    // Leaving the field stops the lasers.
    company.pilot(miner, FlightMode::MoveTo, {}, at.position + Vec3d{1e9, 0.0, 0.0});
    company.simulation.runFor(SimDuration::minutes(1));
    GX_EXPECT(!world.components<MiningControl>().get(miner).active);
    GX_EXPECT(company.journalContains("Fuera del campo"));
}

GX_TEST(Fleet, CompanyTradersTradeWithTheCompanysKnowledge) {
    Company company(3);
    company.give(30'000);
    const EntityId hauler =
        company.buy(content::kShipClassHauler, content::kShipPrices[content::kShipClassHauler]);
    GX_REQUIRE(hauler.isValid());
    const SimTime start = company.simulation.now();
    company.order(hauler, FleetOrder::Trade);
    company.simulation.runFor(SimDuration::hours(4));
    const CompanyTotals& totals = company.sandbox.company().totals;
    GX_EXPECT(totals.purchases > 0);
    GX_EXPECT(totals.sales > 0);
    GX_EXPECT(company.world().components<FleetBrain>().get(hauler).trips >= 3u);
    // The company learnt the prices where its trader went.
    const auto learnt = std::count_if(
        company.sandbox.playerPrices().ports.begin(), company.sandbox.playerPrices().ports.end(),
        [&](const PortPrices& known) { return known.observed > start + SimDuration::hours(1); });
    GX_EXPECT(learnt >= 2);
    company.expectAccountAddsUp();
    company.expectBankBalances();
}

GX_TEST(Fleet, EscortsFollowTheFlagshipAndFightOffRaiders) {
    SandboxConfig config;
    config.pirates = 1;
    config.maxPatrols = 0;
    Company company(0, config);
    company.give(30'000);
    World& world = company.world();
    const EntityId escort =
        company.buy(content::kShipClassEscort, content::kShipPrices[content::kShipClassEscort]);
    GX_REQUIRE(escort.isValid());
    company.order(escort, FleetOrder::Escort);
    company.simulation.runFor(SimDuration::seconds(5));
    const ShipControl& control = world.components<ShipControl>().get(escort);
    GX_EXPECT(control.mode == FlightMode::Approach && control.target == company.sandbox.playerShip());
    GX_EXPECT(world.components<FleetBrain>().get(escort).task == FleetTask::Escorting);

    // A raider without power drifts 20,000 km from the flagship: the escort goes for it.
    const EntityId pirate = world.components<PirateBrain>().entities()[0];
    const Kinematics& player = world.components<Kinematics>().get(company.sandbox.playerShip());
    world.components<Kinematics>().get(pirate) = {
        player.position + Vec3d{2e7, 0.0, 0.0}, player.velocity, {}};
    for (ShipModule& module : world.components<ShipModules>().get(pirate).modules) {
        module.health = module.type == ModuleType::Reactor ? 0.0 : module.health;
    }
    applyModuleEffects(world, pirate);
    bool engaged = false;
    for (int second = 0; second < 600 && world.isAlive(pirate); ++second) {
        company.simulation.runFor(SimDuration::seconds(1));
        engaged = engaged || world.components<FleetBrain>().get(escort).task == FleetTask::Engaging;
    }
    GX_EXPECT(engaged);
    GX_EXPECT(!world.isAlive(pirate));
    GX_EXPECT_EQ(company.sandbox.company().totals.bounties, content::kPirateBounty); // the company collects
    GX_EXPECT(company.journalContains("destruye su objetivo"));
    company.simulation.runFor(SimDuration::seconds(10));
    GX_EXPECT(world.components<FleetBrain>().get(escort).task ==
              FleetTask::Escorting); // back to the flagship
    company.expectAccountAddsUp();
}

GX_TEST(Fleet, LostShipsAreInsuredAndTheDebtFollowsTheCollateral) {
    SandboxConfig config;
    config.pirates = 1;
    config.maxPatrols = 0;
    Company company(0, config);
    World& world = company.world();
    const i64 price = content::kShipPrices[content::kShipClassMiner];
    const EntityId miner = company.buy(content::kShipClassMiner, price / 4);
    GX_REQUIRE(miner.isValid());
    GX_REQUIRE(company.sandbox.company().debt == price - price / 4);

    // A raider waiting far from everything; the miner, nearly wrecked, stopped next to it.
    const EntityId pirate = world.components<PirateBrain>().entities()[0];
    const Vec3d spot{0.0, 0.0, 5e10};
    world.components<Kinematics>().get(pirate) = {spot, {}, {}};
    world.components<ShipControl>().get(pirate).point = spot;
    world.components<PirateBrain>().get(pirate).nextMove = company.simulation.now() + SimDuration::hours(9);
    world.components<Kinematics>().get(miner) = {spot + Vec3d{150'000.0, 0.0, 0.0}, {}, {}};
    ShipControl& control = world.components<ShipControl>().get(miner);
    control.mode = FlightMode::Stop;
    control.arrived = true;
    world.components<ShipModules>().get(miner).modules[0].health = 5.0;
    world.components<ShipModules>().get(miner).lastDamaged = company.simulation.now(); // no repairs meanwhile
    const i64 fund = company.sandbox.mutual().fund;
    for (int second = 0; second < 600 && world.isAlive(miner); ++second) {
        company.simulation.runFor(SimDuration::seconds(1));
    }
    GX_REQUIRE(!world.isAlive(miner));
    const CompanyTotals& totals = company.sandbox.company().totals;
    GX_EXPECT_EQ(totals.shipsLost, 1u);
    GX_EXPECT_EQ(totals.claims, price); // the mutual pays the hull...
    GX_EXPECT(company.sandbox.mutual().fund < fund);
    GX_EXPECT_EQ(company.sandbox.company().debt, 0); // ...and with no hulls left the bank is paid off first
    GX_EXPECT(company.journalContains("Mutua de Fletadores paga"));
    company.expectAccountAddsUp();
    company.expectBankBalances();
}

GX_TEST(Fleet, AnAccountLeftOverdrawnLosesAShip) {
    SandboxConfig config = peaceful();
    config.finance = false; // no credit line to fall back on
    Company company(0, config);
    company.give(20'000);
    World& world = company.world();
    const EntityId miner =
        company.buy(content::kShipClassMiner, content::kShipPrices[content::kShipClassMiner]);
    const EntityId hauler =
        company.buy(content::kShipClassHauler, content::kShipPrices[content::kShipClassHauler]);
    GX_REQUIRE(miner.isValid() && hauler.isValid());
    GX_EXPECT(!world.components<OwnedShip>().get(miner).insured); // no mutual either
    company.give(-(company.sandbox.company().account.credits + 2'000));
    company.simulation.runFor(SimDuration::minutes(2));
    GX_EXPECT(company.sandbox.company().overdrawn);
    GX_EXPECT(world.isAlive(hauler));
    company.simulation.runFor(content::kArrearsGrace);
    // The most valuable hull goes first, at a forced sale's price.
    GX_EXPECT(!world.isAlive(hauler));
    GX_EXPECT(world.isAlive(miner));
    GX_EXPECT_EQ(company.sandbox.company().totals.forcedSales, 1u);
    GX_EXPECT(company.sandbox.company().account.credits > 0);
    GX_EXPECT(company.journalContains("sigue en descubierto"));
    company.expectAccountAddsUp();
}

GX_TEST(Fleet, PremiumsFollowTheCompanysOwnRecord) {
    Company company(0);
    company.give(60'000);
    // With no record of its own the company pays what the mutual charges everybody.
    GX_EXPECT_NEAR(company.sandbox.companyPremiumPerHour(content::kHaulerHullPrice),
                   company.sandbox.premiumPerHour(), 1e-9);
    for (int i = 0; i < 4; ++i) {
        GX_REQUIRE(company.buy(content::kShipClassHauler, content::kShipPrices[content::kShipClassHauler])
                       .isValid());
    }
    // Hours of insured time without a claim (docked, holding): its own record starts to weigh.
    company.simulation.runFor(SimDuration::hours(3));
    const f64 own = company.sandbox.companyPremiumPerHour(content::kHaulerHullPrice);
    GX_EXPECT(own < 0.8 * company.sandbox.premiumPerHour());
    GX_EXPECT(company.sandbox.company().totals.premiums > 0);
    company.expectAccountAddsUp();
    company.expectBankBalances();
}

GX_TEST(Fleet, FleetCommandsAreValidated) {
    Company company(0);
    company.give(30'000);
    World& world = company.world();
    const EntityId miner =
        company.buy(content::kShipClassMiner, content::kShipPrices[content::kShipClassMiner]);
    const EntityId hauler =
        company.buy(content::kShipClassHauler, content::kShipPrices[content::kShipClassHauler]);
    GX_REQUIRE(miner.isValid() && hauler.isValid());
    const EntityId player = company.sandbox.playerShip();
    const EntityId field = company.rockField();
    const EntityId home = company.sandbox.homePort();
    const u64 rejected = company.sandbox.stats().commandsRejected;
    company.simulation.submitCommand(FleetOrderCommand{player, FleetOrder::Hold, {}, {}});    // the flagship
    company.simulation.submitCommand(FleetOrderCommand{hauler, FleetOrder::Mine, field, {}}); // no lasers
    company.simulation.submitCommand(FleetOrderCommand{miner, FleetOrder::Mine, home, {}});   // not a field
    company.simulation.submitCommand(FleetOrderCommand{miner, FleetOrder::Dock, field, {}});  // not a port
    company.simulation.submitCommand(SellShipCommand{player});                                // flown
    company.simulation.submitCommand(MineCommand{player, field, true});                       // no lasers
    company.simulation.submitCommand(LoanCommand{1'000'000, 0});                              // too much
    company.simulation.submitCommand(
        BuyShipCommand{player, content::kShipClassRaider, 0, true}); // not for sale
    company.simulation.submitCommand(FlagshipCommand{home});         // not a ship
    company.simulation.runFor(SimDuration::seconds(1));
    GX_EXPECT_EQ(company.sandbox.stats().commandsRejected, rejected + 9);

    // A valid sale: docked at the station's yards, for 70% of the hull.
    const i64 before = company.sandbox.company().account.credits;
    company.simulation.submitCommand(SellShipCommand{hauler});
    company.simulation.runFor(SimDuration::seconds(1));
    GX_EXPECT(!world.isAlive(hauler));
    GX_EXPECT_EQ(company.sandbox.company().account.credits - before,
                 static_cast<i64>(content::kShipResale * content::kShipPrices[content::kShipClassHauler]));
    // Borrowing against the remaining hull, then paying it back.
    company.simulation.submitCommand(LoanCommand{2'000, 0});
    company.simulation.runFor(SimDuration::seconds(1));
    GX_EXPECT_EQ(company.sandbox.company().debt, 2'000);
    company.simulation.submitCommand(LoanCommand{0, 5'000});
    company.simulation.runFor(SimDuration::seconds(1));
    GX_EXPECT_EQ(company.sandbox.company().debt, 0);
    company.expectAccountAddsUp();
    company.expectBankBalances();
}

GX_TEST(Fleet, AFleetSavesAndLoads) {
    const auto opening = [](Company& company) {
        company.give(30'000);
        const EntityId miner = company.buy(content::kShipClassMiner, 2'000);
        const EntityId hauler =
            company.buy(content::kShipClassHauler, content::kShipPrices[content::kShipClassHauler]);
        const EntityId escort =
            company.buy(content::kShipClassEscort, content::kShipPrices[content::kShipClassEscort]);
        company.order(miner, FleetOrder::Mine, company.rockField());
        company.order(hauler, FleetOrder::Trade);
        company.order(escort, FleetOrder::Escort);
        company.simulation.runFor(SimDuration::minutes(40));
    };
    SandboxConfig config; // with pirates
    Company continuous(3, config);
    opening(continuous);
    continuous.simulation.runFor(SimDuration::minutes(40));

    Company saver(3, config);
    opening(saver);
    const std::vector<std::byte> saved = saver.simulation.saveState();
    JobSystem jobs(0);
    Sandbox sandbox(config);
    Simulation loaded({.seed = config.seed}, jobs);
    sandbox.install(loaded);
    std::string error;
    GX_REQUIRE(loaded.loadState(saved, error));
    GX_EXPECT(loaded.saveState() == saved);
    GX_EXPECT(sandbox.fields() == saver.sandbox.fields());
    loaded.runFor(SimDuration::minutes(40));
    GX_EXPECT_EQ(loaded.stateHash(), continuous.simulation.stateHash());
    GX_EXPECT_EQ(sandbox.company().account.credits, continuous.sandbox.company().account.credits);
}

GX_TEST(Fleet, TheCompanysBooksAddUpThroughAWar) {
    // Pirates: losses, claims, repairs, fights; a financed fleet of every kind.
    SandboxConfig config;
    config.pirates = 6;
    Company company(3, config);
    company.give(12'000);
    const EntityId miner = company.buy(content::kShipClassMiner, 2'000);
    const EntityId hauler = company.buy(content::kShipClassHauler, 3'000);
    const EntityId escort =
        company.buy(content::kShipClassEscort, content::kShipPrices[content::kShipClassEscort]);
    GX_REQUIRE(miner.isValid() && hauler.isValid() && escort.isValid());
    company.order(miner, FleetOrder::Mine, company.rockField());
    company.order(hauler, FleetOrder::Trade);
    company.order(escort, FleetOrder::Escort);
    const Catalog english = loadEnglish();
    std::set<std::string> missing;
    for (int step = 0; step < 6 * 6; ++step) { // 6 h, looking every 10 min
        company.simulation.runFor(SimDuration::minutes(10));
        company.expectAccountAddsUp();
        company.expectBankBalances();
        for (const JournalEntry& entry : company.sandbox.journal()) {
            std::vector<std::string> sources;
            collectSources(entry.text, sources);
            for (const std::string& source : sources) {
                if (english.find(source) == nullptr) {
                    missing.insert(source);
                }
            }
        }
    }
    for (const std::string& source : missing) {
        std::printf("untranslated: %s\n", source.c_str());
    }
    GX_EXPECT(missing.empty()); // everything the company's ships report is translated too
    GX_EXPECT(company.sandbox.company().totals.wages > 0);
    GX_EXPECT(company.sandbox.company().totals.interest > 0);
}

GX_TEST(Fleet, AFleetIsDeterministicAcrossThreadCounts) {
    const auto run = [](u32 workers) {
        SandboxConfig config; // with pirates
        Company company(workers, config);
        company.give(30'000);
        const EntityId miner = company.buy(content::kShipClassMiner, 2'000);
        const EntityId other = company.buy(content::kShipClassMiner, 2'000);
        const EntityId hauler =
            company.buy(content::kShipClassHauler, content::kShipPrices[content::kShipClassHauler]);
        const EntityId escort =
            company.buy(content::kShipClassEscort, content::kShipPrices[content::kShipClassEscort]);
        company.order(miner, FleetOrder::Mine, company.rockField());
        company.order(other, FleetOrder::Mine, company.rockField());
        company.order(hauler, FleetOrder::Trade);
        company.order(escort, FleetOrder::Escort, miner);
        company.simulation.runFor(SimDuration::hours(2));
        return company.simulation.stateHash();
    };
    const u64 serial = run(0);
    GX_EXPECT_EQ(run(3), serial);
    GX_EXPECT_EQ(run(7), serial);
}
