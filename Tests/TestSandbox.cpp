#include "Tests/TestFramework.h"

#include "Engine/Jobs/JobSystem.h"
#include "Game/Presentation/SystemSnapshot.h"
#include "Game/Sandbox/Content.h"
#include "Game/Sandbox/Sandbox.h"
#include "Simulation/Economy/Economy.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Bodies/CelestialBody.h"

#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace gx;

namespace {

// No pirates: for tests about flying, not fighting.
SandboxConfig peaceful() {
    SandboxConfig config;
    config.pirates = 0;
    return config;
}

// Same setup as the graphical client, without any window.
struct Session {
    explicit Session(u32 workers, const SandboxConfig& config = {})
        : jobs(workers), sandbox(config), simulation({.seed = config.seed}, jobs) {
        sandbox.install(simulation);
    }
    JobSystem jobs;
    Sandbox sandbox;
    Simulation simulation;

    void pilot(FlightMode mode, EntityId target = {}, Vec3d point = {}, Vec3d thrust = {}) {
        simulation.submitCommand(PilotCommand{sandbox.playerShip(), mode, target, point, thrust});
    }
    [[nodiscard]] const ShipControl& playerControl() {
        return simulation.world().components<ShipControl>().get(sandbox.playerShip());
    }
    [[nodiscard]] bool journalContains(const std::string& text) const {
        for (const JournalEntry& entry : sandbox.journal()) {
            if (entry.text.find(text) != std::string::npos) {
                return true;
            }
        }
        return false;
    }
    // The nearest port that is not next to the starting station (a trip of hours, not days).
    [[nodiscard]] EntityId destinationPort() {
        const World& world = simulation.world();
        const Vec3d player = world.components<Kinematics>().get(sandbox.playerShip()).position;
        EntityId best;
        f64 bestDistance = std::numeric_limits<f64>::max();
        for (const EntityId port : sandbox.ports()) {
            const f64 d = length(bodyStateAt(world, port, simulation.now()).position - player);
            if (d > 1e9 && d < bestDistance) {
                bestDistance = d;
                best = port;
            }
        }
        return best;
    }
};

} // namespace

GX_TEST(Sandbox, NewGameHasPlayerHaulersPiratesAndPorts) {
    Session session(0);
    session.sandbox.populate(session.simulation);
    const World& world = session.simulation.world();
    GX_REQUIRE(world.isAlive(session.sandbox.playerShip()));
    GX_EXPECT_EQ(world.components<ShipIdentity>().get(session.sandbox.playerShip()).faction,
                 static_cast<u32>(content::kFactionPlayer));
    GX_EXPECT_EQ(world.components<HaulerBrain>().size(),
                 static_cast<usize>(session.sandbox.config().haulers));
    GX_EXPECT_EQ(world.components<PirateBrain>().size(),
                 static_cast<usize>(session.sandbox.config().pirates));
    for (const EntityId pirate : world.components<PirateBrain>().entities()) {
        GX_EXPECT(!world.components<SensorSuite>().get(pirate).transponderOn); // raiders run silent
    }
    GX_EXPECT(world.components<CombatControl>().contains(session.sandbox.playerShip())); // armed
    GX_EXPECT(session.sandbox.ports().size() >= 6); // 4-9 planets + 2 stations
    GX_EXPECT(!session.sandbox.systemName().empty());
    GX_EXPECT_EQ(session.sandbox.journal().size(), 1u);
    GX_EXPECT(session.playerControl().arrived); // starts docked at the main station
}

GX_TEST(Sandbox, HaulersTravelWithoutThePlayer) {
    Session session(3);
    session.sandbox.populate(session.simulation);
    session.simulation.runFor(SimDuration::minutes(30));
    GX_EXPECT(session.sandbox.stats().haulerDepartures >= session.sandbox.config().haulers);
    GX_EXPECT(session.sandbox.stats().arrivals > 0);
    GX_EXPECT(session.journalContains("atraca en"));
}

GX_TEST(Sandbox, PlayerFliesToAPortByCommand) {
    Session session(3, peaceful());
    session.sandbox.populate(session.simulation);
    const EntityId destination = session.destinationPort();
    session.pilot(FlightMode::Approach, destination);
    session.simulation.runFor(SimDuration::minutes(1));
    GX_EXPECT(session.playerControl().target == destination);
    GX_EXPECT(!session.playerControl().arrived);

    for (int slice = 0; slice < 12 && !session.playerControl().arrived; ++slice) {
        session.simulation.runFor(SimDuration::minutes(5)); // real-time scale: minutes per trip
    }
    GX_EXPECT(session.playerControl().arrived);
    GX_EXPECT(session.journalContains("ha llegado a"));
}

GX_TEST(Sandbox, PilotCommandsAreValidated) {
    Session session(0);
    session.sandbox.populate(session.simulation);
    const World& world = session.simulation.world();
    const EntityId hauler = world.components<HaulerBrain>().entities()[0];
    const f64 nan = std::numeric_limits<f64>::quiet_NaN();

    session.simulation.submitCommand(PilotCommand{hauler, FlightMode::Stop, {}, {}, {}}); // not the player's
    session.pilot(FlightMode::Approach, session.sandbox.playerShip());                    // itself
    session.pilot(FlightMode::Approach, EntityId{9999, 0});                               // nonexistent
    session.pilot(FlightMode::MoveTo, {}, Vec3d{nan, 0.0, 0.0});                          // not finite
    session.pilot(FlightMode::Count);                                                     // invalid mode
    session.simulation.runFor(SimDuration::minutes(1));
    GX_EXPECT_EQ(session.sandbox.stats().commandsRejected, 5u);
    GX_EXPECT(session.playerControl().mode == FlightMode::Approach); // unchanged: still docked
}

GX_TEST(Sandbox, ManualPilotingSwitchesToTacticalFlightRate) {
    Session session(0);
    session.sandbox.populate(session.simulation);
    const auto flightPeriod = [&] {
        return session.simulation.scheduler().system(session.sandbox.flightSystem()).desc.period;
    };
    GX_EXPECT(flightPeriod() == session.sandbox.config().strategicFlightPeriod);
    session.pilot(FlightMode::Manual, {}, {}, Vec3d{0.0, 1.0, 0.0});
    session.simulation.runFor(SimDuration::seconds(2));
    GX_EXPECT(flightPeriod() == session.sandbox.config().tacticalFlightPeriod);
    const Kinematics& state =
        session.simulation.world().components<Kinematics>().get(session.sandbox.playerShip());
    GX_EXPECT(length(state.acceleration) > 0.0);
    session.pilot(FlightMode::Stop);
    session.simulation.runFor(SimDuration::seconds(2));
    GX_EXPECT(flightPeriod() == session.sandbox.config().strategicFlightPeriod);
}

GX_TEST(Sandbox, SessionIsDeterministicAcrossThreadCounts) {
    const auto play = [](u32 workers) {
        Session session(workers);
        session.sandbox.populate(session.simulation);
        const EntityId destination = session.destinationPort();
        session.simulation.runFor(SimDuration::minutes(3));
        session.pilot(FlightMode::Manual, {}, {}, Vec3d{1.0, 0.0, 0.0});
        session.simulation.runFor(SimDuration::seconds(30));
        session.pilot(FlightMode::Approach, destination);
        session.simulation.runFor(SimDuration::minutes(20));
        return session.simulation.stateHash();
    };
    const u64 reference = play(0);
    GX_EXPECT_EQ(play(3), reference);
    GX_EXPECT_EQ(play(11), reference);
}

GX_TEST(Sandbox, SaveLoadContinuesIdentically) {
    const auto opening = [](Session& session) {
        session.sandbox.populate(session.simulation);
        session.simulation.runFor(SimDuration::minutes(10));
        session.pilot(FlightMode::Approach, session.destinationPort()); // in flight when saved
        session.simulation.runFor(SimDuration::seconds(30));
    };
    Session continuous(3);
    opening(continuous);
    continuous.simulation.runFor(SimDuration::minutes(20));

    Session saver(3);
    opening(saver);
    const std::vector<std::byte> saved = saver.simulation.saveState();

    Session loaded(0); // no populate: everything comes from the save
    std::string error;
    GX_REQUIRE(loaded.simulation.loadState(saved, error));
    GX_EXPECT(loaded.simulation.saveState() == saved);
    GX_EXPECT_EQ(loaded.sandbox.systemName(), saver.sandbox.systemName());
    GX_EXPECT(loaded.sandbox.ports() == saver.sandbox.ports());
    loaded.simulation.runFor(SimDuration::minutes(20));
    GX_EXPECT_EQ(loaded.simulation.stateHash(), continuous.simulation.stateHash());
}

GX_TEST(Sandbox, SnapshotDescribesTheWorld) {
    Session session(0);
    session.sandbox.populate(session.simulation);
    session.simulation.runFor(SimDuration::minutes(30) + SimDuration::milliseconds(500));
    SnapshotBuilder builder;
    SystemSnapshot snapshot;
    builder.build(session.simulation, session.sandbox, snapshot);
    GX_EXPECT_EQ(snapshot.ships.size(), session.simulation.world().components<ShipIdentity>().size());
    GX_EXPECT(snapshot.playerAlive);
    GX_EXPECT_EQ(snapshot.playerModules.size(), content::kCourierModules.size());
    GX_EXPECT_EQ(snapshot.bodies.size(), session.simulation.world().components<CelestialBody>().size());
    const ShipView* player = snapshot.findShip(session.sandbox.playerShip());
    GX_REQUIRE(player != nullptr);
    GX_EXPECT(player->isPlayer);
    for (const BodyView& body : snapshot.bodies) {
        GX_EXPECT(body.kind == BodyKind::Star ||
                  (body.orbitPath != nullptr && body.orbitPath->size() == SnapshotBuilder::kOrbitPathPoints));
    }
    // Ships are extrapolated from their last flight step to "now".
    const Kinematics& state =
        session.simulation.world().components<Kinematics>().get(session.sandbox.playerShip());
    const f64 sinceFlight = (session.simulation.now() -
                             session.simulation.scheduler().system(session.sandbox.flightSystem()).lastRun)
                                .toSeconds();
    GX_EXPECT(sinceFlight > 0.0);
    GX_EXPECT_NEAR(length(player->position - (state.position + state.velocity * sinceFlight)), 0.0, 1e-3);
}

GX_TEST(Sandbox, PiratesHuntHaulersAndLossesAreReplaced) {
    SandboxConfig config;
    config.pirates = 4;
    Session session(3, config);
    session.sandbox.populate(session.simulation);
    session.simulation.runFor(SimDuration::minutes(60));
    const SandboxStats& stats = session.sandbox.stats();
    GX_EXPECT(stats.hunts > 0);
    GX_EXPECT(session.sandbox.combat().stats().hits > 0);
    GX_EXPECT(stats.haulersLost + stats.piratesLost + stats.piratesLeft + stats.playerDeaths > 0);
    GX_EXPECT(stats.spawns > 0); // losses are replaced
    const World& world = session.simulation.world();
    GX_EXPECT(world.components<HaulerBrain>().size() <= config.haulers);
    GX_EXPECT(world.components<HaulerBrain>().size() + 3 >= config.haulers); // one newcomer a minute
}

GX_TEST(Sandbox, EngageCommandsAreValidated) {
    Session session(0, peaceful());
    session.sandbox.populate(session.simulation);
    session.simulation.runFor(SimDuration::seconds(3)); // a few scans: nearby haulers become contacts
    const FactionPicture& picture = session.sandbox.sensors().picture(content::kFactionPlayer);
    GX_REQUIRE(!picture.contacts.empty());
    const u32 track = picture.contacts.front().trackId;
    const EntityId player = session.sandbox.playerShip();
    const World& world = session.simulation.world();

    session.simulation.submitCommand(EngageCommand{player, 99'999, true, true}); // unknown track
    session.simulation.submitCommand(EngageCommand{player, 0, true, false});     // fire at nothing
    const EntityId hauler = world.components<HaulerBrain>().entities()[0];
    session.simulation.submitCommand(EngageCommand{hauler, track, true, true}); // not the player's
    session.simulation.runFor(SimDuration::seconds(1));
    GX_EXPECT_EQ(session.sandbox.stats().commandsRejected, 3u);
    GX_EXPECT_EQ(world.components<CombatControl>().get(player).targetTrack, 0u);

    session.simulation.submitCommand(EngageCommand{player, track, true, true});
    session.simulation.runFor(SimDuration::seconds(1));
    GX_EXPECT_EQ(world.components<CombatControl>().get(player).targetTrack, track);
    GX_EXPECT(session.playerControl().mode == FlightMode::Pursue);
    GX_EXPECT_EQ(session.playerControl().track, track);
    GX_EXPECT(session.sandbox.tactical()); // fighting: fine flight and combat steps
    GX_EXPECT(session.journalContains("Fuego sobre"));

    session.simulation.submitCommand(EngageCommand{player, 0, false, false});
    session.simulation.runFor(SimDuration::seconds(1));
    GX_EXPECT_EQ(world.components<CombatControl>().get(player).targetTrack, 0u);
    GX_EXPECT(session.playerControl().mode == FlightMode::Pursue); // cease fire does not change course
    GX_EXPECT(session.journalContains("Alto el fuego"));
}

GX_TEST(Sandbox, StationsRepairAndDamageControlRestoresPower) {
    Session session(0, peaceful());
    session.sandbox.populate(session.simulation);
    World& world = session.simulation.world();
    const EntityId player = session.sandbox.playerShip();
    const auto modules = [&]() -> ShipModules& { return world.components<ShipModules>().get(player); };

    // Docked at the home station: everything is patched up at 2% per second (paid: give it the money).
    world.components<Wallet>().get(player).credits = 100'000;
    for (ShipModule& module : modules().modules) {
        module.health = module.maxHealth * 0.3;
    }
    session.simulation.runFor(SimDuration::seconds(40));
    for (const ShipModule& module : modules().modules) {
        GX_EXPECT_NEAR(module.fraction(), 1.0, 1e-9);
    }

    // Adrift with a wrecked reactor: no power until damage control gets it back to 10% (10 s + 40 s).
    session.pilot(FlightMode::Stop);
    session.simulation.runFor(SimDuration::seconds(1));
    ShipModule& reactor = modules().modules[1];
    GX_REQUIRE(reactor.type == ModuleType::Reactor);
    reactor.health = 0.0;
    modules().lastDamaged = session.simulation.now();
    applyModuleEffects(world, player);
    GX_EXPECT(!hasPower(modules()));
    GX_EXPECT_EQ(world.components<ShipDrive>().get(player).maxAcceleration, 0.0);
    session.simulation.runFor(SimDuration::seconds(30));
    GX_EXPECT(!hasPower(modules()));
    session.simulation.runFor(SimDuration::seconds(30));
    GX_EXPECT(hasPower(modules()));
    GX_EXPECT(world.components<ShipDrive>().get(player).maxAcceleration > 0.0);
    session.simulation.runFor(SimDuration::minutes(5));
    GX_EXPECT_NEAR(modules().modules[1].fraction(), content::kDamageControlCap, 1e-9); // limp-home level
}

GX_TEST(Sandbox, PlayerIsReplacedAfterBeingDestroyed) {
    SandboxConfig config;
    config.haulers = 0;
    config.pirates = 1;
    Session session(0, config);
    session.sandbox.populate(session.simulation);
    World& world = session.simulation.world();
    const EntityId player = session.sandbox.playerShip();
    const EntityId pirate = world.components<PirateBrain>().entities()[0];

    // Put the raider and the player (transponder on, nearly wrecked) together, far from any station.
    session.pilot(FlightMode::Stop);
    session.simulation.runFor(SimDuration::milliseconds(10));
    const Vec3d spot{0.0, 0.0, 5e10};
    world.components<Kinematics>().get(pirate) = {spot, {}, {}};
    world.components<ShipControl>().get(pirate).point = spot;
    world.components<Kinematics>().get(player) = {spot + Vec3d{150'000.0, 0.0, 0.0}, {}, {}};
    world.components<ShipModules>().get(player).modules[0].health = 5.0;
    session.simulation.runFor(SimDuration::seconds(20));

    GX_EXPECT(!world.isAlive(player));
    GX_EXPECT_EQ(session.sandbox.stats().playerDeaths, 1u);
    GX_EXPECT(session.journalContains("destruida"));
    GX_EXPECT(session.sandbox.stats().hunts >= 1);
    session.simulation.runFor(SimDuration::seconds(15)); // replacement after 10 s
    const EntityId replacement = session.sandbox.playerShip();
    GX_REQUIRE(replacement.isValid() && world.isAlive(replacement));
    GX_EXPECT(replacement != player);
    GX_EXPECT(session.playerControl().target == session.sandbox.homePort());
    GX_EXPECT(session.journalContains("Una nave nueva"));
}

namespace {

// Tonnes of each good in the markets and in the holds, as the economy inspector would sum them.
struct GoodsCensus {
    std::vector<f64> produced;
    std::vector<f64> consumed;
    std::vector<f64> stock;
    std::vector<f64> cargo;
};

GoodsCensus census(const World& world) {
    GoodsCensus result;
    result.produced.assign(content::kGoodCount, 0.0);
    result.consumed.assign(content::kGoodCount, 0.0);
    result.stock.assign(content::kGoodCount, 0.0);
    result.cargo.assign(content::kGoodCount, 0.0);
    for (const Market& market : world.components<Market>().values()) {
        for (const MarketGood& good : market.goods) {
            result.produced[good.good] += good.produced;
            result.consumed[good.good] += good.consumed;
            result.stock[good.good] += good.stock;
        }
    }
    for (const CargoHold& hold : world.components<CargoHold>().values()) {
        for (const CargoItem& item : hold.items) {
            result.cargo[item.good] += item.tonnes;
        }
    }
    return result;
}

} // namespace

GX_TEST(Sandbox, EveryPortHasAMarketAndEveryGoodASupplier) {
    Session session(0, peaceful());
    session.sandbox.populate(session.simulation);
    const World& world = session.simulation.world();
    for (const EntityId port : session.sandbox.ports()) {
        GX_EXPECT(world.components<Market>().contains(port));
    }
    for (GoodId good = 0; good < content::kGoodCount; ++good) {
        f64 production = 0.0;
        f64 consumption = 0.0;
        for (const Market& market : world.components<Market>().values()) {
            production += productionRate(market, good);
            consumption += consumptionRate(market, good);
        }
        GX_EXPECT(consumption > 0.0);
        GX_EXPECT(production >= consumption * content::kSupplyMargin - 1e-6); // no structural famine
    }
    const EntityId player = session.sandbox.playerShip();
    GX_EXPECT_EQ(world.components<Wallet>().get(player).credits, content::kPlayerStartCredits);
    GX_EXPECT_EQ(world.components<CargoHold>().get(player).capacity,
                 content::kCargoCapacity[content::kShipClassCourier]);
    GX_EXPECT_EQ(session.sandbox.playerPrices().ports.size(),
                 session.sandbox.ports().size()); // opening bulletin
}

GX_TEST(Sandbox, GoodsAreConservedThroughTradeAndLosses) {
    Session session(3); // with pirates: goods also leave the economy with destroyed ships
    session.sandbox.populate(session.simulation);
    session.simulation.runFor(SimDuration::hours(2));
    const GoodsCensus now = census(session.simulation.world());
    const SandboxStats& stats = session.sandbox.stats();
    for (GoodId good = 0; good < content::kGoodCount; ++good) {
        const f64 lost = static_cast<f64>(stats.cargoLost[good]);
        GX_EXPECT_NEAR(session.sandbox.initialStock()[good] + now.produced[good] - now.consumed[good] - lost,
                       now.stock[good] + now.cargo[good], 1e-6);
    }
    GX_EXPECT(stats.haulerTrades > 0);
    GX_EXPECT(stats.tonnesDelivered > 0);
}

GX_TEST(Sandbox, HaulersTradeAtAProfit) {
    Session session(3, peaceful());
    session.sandbox.populate(session.simulation);
    const World& world = session.simulation.world();
    session.simulation.runFor(SimDuration::hours(4));
    // Net worth: money plus the cargo being carried (at base prices), after taxes and wages.
    f64 worth = 0.0;
    for (const EntityId hauler : world.components<HaulerBrain>().entities()) {
        worth += static_cast<f64>(world.components<Wallet>().get(hauler).credits);
        for (const CargoItem& item : world.components<CargoHold>().get(hauler).items) {
            worth += item.tonnes * session.sandbox.economy().basePrice(item.good);
        }
    }
    GX_EXPECT(worth > static_cast<f64>(content::kHaulerStartCredits * session.sandbox.config().haulers));
    GX_EXPECT(session.sandbox.stats().wagesPaid > 0);
    GX_EXPECT(session.sandbox.stats().tonnesDelivered > 1'000);
}

GX_TEST(Sandbox, PiratesDisruptTheSupplyChain) {
    // Causality (prompt §30): losses on the routes mean fewer deliveries and cargo gone for good.
    const auto run = [](u32 pirates) {
        SandboxConfig config;
        config.pirates = pirates;
        Session session(3, config);
        session.sandbox.populate(session.simulation);
        // Eight hours: over shorter runs the newcomers' fresh capital masks the losses (measured).
        session.simulation.runFor(SimDuration::hours(8));
        u64 lost = 0;
        for (const u64 tonnes : session.sandbox.stats().cargoLost) {
            lost += tonnes;
        }
        return std::pair<u64, u64>{session.sandbox.stats().tonnesDelivered, lost};
    };
    const auto [calmDelivered, calmLost] = run(0);
    const auto [raidedDelivered, raidedLost] = run(6);
    GX_EXPECT_EQ(calmLost, 0u);
    GX_EXPECT(raidedLost > 0);
    GX_EXPECT(raidedDelivered < calmDelivered);
}

GX_TEST(Sandbox, PlayerBuysAndSellsWhenDocked) {
    Session session(0, peaceful());
    session.sandbox.populate(session.simulation);
    World& world = session.simulation.world();
    const EntityId player = session.sandbox.playerShip();
    const EntityId port = session.sandbox.dockedPort(world, player);
    GX_REQUIRE(port.isValid()); // starts docked at the home station
    const Market& market = world.components<Market>().get(port);
    GoodId good = market.goods.front().good;
    for (const MarketGood& candidate : market.goods) {
        good = candidate.stock > market.find(good)->stock ? candidate.good : good;
    }

    session.simulation.submitCommand(TradeCommand{player, good, 5});
    session.simulation.runFor(SimDuration::seconds(1));
    const i64 afterBuying = world.components<Wallet>().get(player).credits;
    GX_EXPECT_EQ(world.components<CargoHold>().get(player).amount(good), 5u);
    GX_EXPECT(afterBuying < content::kPlayerStartCredits);
    GX_EXPECT(session.journalContains("Compras 5 t"));

    session.simulation.submitCommand(TradeCommand{player, good, -5});
    session.simulation.runFor(SimDuration::seconds(1));
    const i64 afterSelling = world.components<Wallet>().get(player).credits;
    GX_EXPECT_EQ(world.components<CargoHold>().get(player).amount(good), 0u);
    GX_EXPECT(afterSelling > afterBuying);
    GX_EXPECT(afterSelling < content::kPlayerStartCredits); // the spread is the market's cut
    GX_EXPECT_EQ(session.sandbox.stats().playerTrades, 2u);

    // Undocked, or a good the port does not trade: rejected, with a reason in the journal.
    const u64 rejectedBefore = session.sandbox.stats().commandsRejected;
    GoodId untraded = content::kGoodCount;
    for (GoodId g = 0; g < content::kGoodCount && untraded == content::kGoodCount; ++g) {
        untraded = market.find(g) == nullptr ? g : untraded;
    }
    if (untraded < content::kGoodCount) {
        session.simulation.submitCommand(TradeCommand{player, untraded, 1});
    }
    session.simulation.submitCommand(TradeCommand{player, good, 1'000'000}); // capped by hold and credits
    session.pilot(FlightMode::Stop);
    session.simulation.runFor(SimDuration::seconds(1));
    session.simulation.submitCommand(TradeCommand{player, good, 1});
    session.simulation.runFor(SimDuration::seconds(1));
    GX_EXPECT(session.journalContains("atracado en un puerto"));
    GX_EXPECT_EQ(session.sandbox.stats().commandsRejected,
                 rejectedBefore + (untraded < content::kGoodCount ? 2u : 1u));
    GX_EXPECT(world.components<CargoHold>().get(player).used() <=
              world.components<CargoHold>().get(player).capacity);
    GX_EXPECT(world.components<Wallet>().get(player).credits >= 0);
}

GX_TEST(Sandbox, StationsPublishTheTradersPriceBulletin) {
    Session session(3, peaceful());
    session.sandbox.populate(session.simulation);
    World& world = session.simulation.world();
    session.simulation.runFor(
        SimDuration::minutes(30)); // traders see fresher prices than the opening bulletin
    // Fly to the other station.
    EntityId other;
    for (const EntityId port : session.sandbox.ports()) {
        if (port != session.sandbox.homePort() &&
            world.components<CelestialBody>().get(port).kind == BodyKind::Station) {
            other = port;
        }
    }
    GX_REQUIRE(other.isValid());
    session.pilot(FlightMode::Approach, other);
    for (int slice = 0;
         slice < 30 && !(session.playerControl().arrived && session.playerControl().target == other);
         ++slice) {
        session.simulation.runFor(SimDuration::minutes(1));
    }
    GX_REQUIRE(session.playerControl().arrived);
    GX_EXPECT(session.journalContains("Boletín de precios"));
    u32 fresher = 0;
    for (const PortPrices& known : session.sandbox.playerPrices().ports) {
        fresher += known.observed > SimTime::epoch() ? 1u : 0u;
    }
    GX_EXPECT(fresher > 1); // not just the station it docked at
}

namespace {

// Puts a hauler right next to the player (same velocity), disabled or not, with some cargo.
EntityId parkHaulerNextToPlayer(Session& session, usize which, bool disabled, u32 water) {
    World& world = session.simulation.world();
    const EntityId player = session.sandbox.playerShip();
    const EntityId hauler = world.components<HaulerBrain>().entities()[which];
    const Kinematics& mine = world.components<Kinematics>().get(player);
    world.components<Kinematics>().get(hauler) = {
        mine.position + Vec3d{1'000.0 * (which + 1), 0.0, 0.0}, mine.velocity, {}};
    world.components<ShipControl>().get(hauler) = {}; // drifting
    world.components<HaulerBrain>().get(hauler).departAt = session.simulation.now() + SimDuration::hours(5);
    world.components<CargoHold>().get(hauler).items = {{content::kGoodWater, water}};
    if (disabled) {
        for (ShipModule& module : world.components<ShipModules>().get(hauler).modules) {
            module.health = module.type == ModuleType::Reactor ? 0.0 : module.health;
        }
        world.components<ShipModules>().get(hauler).lastDamaged =
            session.simulation.now(); // no damage control yet
        applyModuleEffects(world, hauler);
    }
    return hauler;
}

u32 trackOf(const Session& session, EntityId target) {
    for (const SensorContact& contact : session.sandbox.sensors().picture(content::kFactionPlayer).contacts) {
        if (contact.target == target && !contact.ghost) {
            return contact.trackId;
        }
    }
    return 0;
}

} // namespace

GX_TEST(Sandbox, TradesPayTaxToTheAuthority) {
    Session session(0, peaceful());
    session.sandbox.populate(session.simulation);
    const World& world = session.simulation.world();
    const EntityId player = session.sandbox.playerShip();
    const Market& market = world.components<Market>().get(session.sandbox.dockedPort(world, player));
    const i64 treasury = session.sandbox.treasury();
    const i64 price =
        buyPrice(market.goods.front(), session.sandbox.economy().basePrice(market.goods.front().good));
    session.simulation.submitCommand(TradeCommand{player, market.goods.front().good, 1});
    session.simulation.runFor(SimDuration::milliseconds(10));
    const i64 tax = session.sandbox.treasury() - treasury;
    GX_EXPECT_EQ(tax, std::llround(static_cast<f64>(price) * content::kTradeTaxRate));
    GX_EXPECT_EQ(world.components<Wallet>().get(player).credits, content::kPlayerStartCredits - price - tax);
    GX_EXPECT(session.journalContains("de impuestos"));
}

GX_TEST(Sandbox, StationRepairsAreChargedAndNeedMoney) {
    Session session(0, peaceful());
    session.sandbox.populate(session.simulation);
    World& world = session.simulation.world();
    const EntityId player = session.sandbox.playerShip();
    ShipModule& structure = world.components<ShipModules>().get(player).modules[0];
    structure.health = structure.maxHealth - 100.0; // 100 points to repair
    const i64 treasury = session.sandbox.treasury();
    session.simulation.runFor(SimDuration::seconds(30));
    const i64 paid = content::kPlayerStartCredits - world.components<Wallet>().get(player).credits;
    GX_EXPECT_NEAR(structure.fraction(), 1.0, 1e-9);
    GX_EXPECT(std::abs(paid - static_cast<i64>(100.0 * content::kRepairCostPerPoint)) <=
              12); // per-second rounding
    GX_EXPECT_EQ(session.sandbox.treasury() - treasury, paid);

    // Broke: no repairs.
    world.components<Wallet>().get(player).credits = 0;
    structure.health = structure.maxHealth - 100.0;
    session.simulation.runFor(SimDuration::seconds(30));
    GX_EXPECT_NEAR(structure.health, structure.maxHealth - 100.0, 1e-9);
}

GX_TEST(Sandbox, BrokeTradersRetireAndAreReplaced) {
    Session session(0, peaceful());
    session.sandbox.populate(session.simulation);
    World& world = session.simulation.world();
    const EntityId hauler = world.components<HaulerBrain>().entities()[0];
    world.components<Wallet>().get(hauler).credits = -1;
    world.components<HaulerBrain>().get(hauler).departAt = session.simulation.now();
    world.components<ShipControl>().get(hauler).arrived = true;
    session.simulation.runFor(SimDuration::seconds(20));
    GX_EXPECT(!world.isAlive(hauler));
    GX_EXPECT_EQ(session.sandbox.stats().bankruptcies, 1u);
    GX_EXPECT(session.journalContains("quiebra"));
    session.simulation.runFor(SimDuration::seconds(70)); // a newcomer after the respawn delay
    GX_EXPECT_EQ(world.components<HaulerBrain>().size(),
                 static_cast<usize>(session.sandbox.config().haulers));
    GX_EXPECT(session.sandbox.stats().wagesPaid > 0);
}

GX_TEST(Sandbox, DestroyingAPiratePaysABounty) {
    SandboxConfig config;
    config.haulers = 0;
    config.pirates = 1;
    Session session(0, config);
    session.sandbox.populate(session.simulation);
    World& world = session.simulation.world();
    const EntityId player = session.sandbox.playerShip();
    const EntityId pirate = world.components<PirateBrain>().entities()[0];
    session.pilot(FlightMode::Stop);
    session.simulation.runFor(SimDuration::milliseconds(10));
    const Vec3d spot{0.0, 0.0, 5e10};
    world.components<Kinematics>().get(player) = {spot, {}, {}};
    world.components<Kinematics>().get(pirate) = {spot + Vec3d{150'000.0, 0.0, 0.0}, {}, {}};
    world.components<ShipControl>().get(pirate).point = spot + Vec3d{150'000.0, 0.0, 0.0};
    world.components<ShipModules>().get(pirate).modules[0].health = 1.0;
    session.simulation.runFor(SimDuration::seconds(2)); // the player's sensors pick it up
    const u32 track = trackOf(session, pirate);
    GX_REQUIRE(track != 0);
    session.simulation.submitCommand(EngageCommand{player, track, true, false});
    session.simulation.runFor(SimDuration::seconds(20));
    GX_EXPECT(!world.isAlive(pirate));
    GX_EXPECT_EQ(session.sandbox.stats().bountiesPaid, content::kPirateBounty);
    GX_EXPECT(world.components<Wallet>().get(player).credits >=
              content::kPlayerStartCredits + content::kPirateBounty);
    GX_EXPECT_NEAR(session.sandbox.reputation(), content::kReputationPirateKill, 1e-9);
    GX_EXPECT(session.journalContains("Recompensa"));
}

GX_TEST(Sandbox, OnlyIdentifiedAttacksCostReputation) {
    const auto attack = [](bool transponder, f64 range) {
        SandboxConfig config;
        config.pirates = 0;
        Session session(0, config);
        session.sandbox.populate(session.simulation);
        World& world = session.simulation.world();
        const EntityId player = session.sandbox.playerShip();
        session.simulation.submitCommand(SensorCommand{player, false, transponder});
        session.pilot(FlightMode::Stop);
        session.simulation.runFor(SimDuration::milliseconds(10));
        const Vec3d spot{0.0, 0.0, 5e10};
        world.components<Kinematics>().get(player) = {spot, {}, {}};
        const EntityId hauler = world.components<HaulerBrain>().entities()[0];
        world.components<Kinematics>().get(hauler) = {spot + Vec3d{range, 0.0, 0.0}, {}, {}};
        world.components<ShipControl>().get(hauler) = {};
        world.components<HaulerBrain>().get(hauler).departAt =
            session.simulation.now() + SimDuration::hours(5);
        session.simulation.runFor(SimDuration::seconds(30)); // old sightings of the player time out
        // A precise passive lock without radar (which would give the shooter away): a test-only sensor
        // upgrade, so that the railgun hits a quiet target at long range and the test is about witnesses.
        world.components<SensorSuite>().get(player).passiveSensitivity = 1e15;
        const u32 track = trackOf(session, hauler);
        if (track == 0) {
            return 999.0;
        }
        session.simulation.submitCommand(EngageCommand{player, track, true, false});
        session.simulation.runFor(SimDuration::seconds(10));
        GX_EXPECT(session.sandbox.combat().stats().hits > 0);
        return session.sandbox.reputation();
    };
    // Transponder on at 150 km: the victim's network knows exactly who it is.
    GX_EXPECT_NEAR(attack(true, 150'000.0), content::kReputationHit, 0.2); // recovering slowly since
    // Transponder off, idle, railgun from 2,000 km: the victim classifies a courier but cannot name it.
    GX_EXPECT_NEAR(attack(false, 2'000'000.0), 0.0, 1e-9);
}

GX_TEST(Sandbox, BoardingTakesCargoAndCostsReputation) {
    Session session(0, peaceful());
    session.sandbox.populate(session.simulation);
    World& world = session.simulation.world();
    const EntityId player = session.sandbox.playerShip();
    const EntityId powered = parkHaulerNextToPlayer(session, 0, false, 10);
    const EntityId victim = parkHaulerNextToPlayer(session, 1, true, 50);
    const EntityId second = parkHaulerNextToPlayer(session, 2, true, 0);
    session.simulation.runFor(
        SimDuration::milliseconds(1'100)); // their transponders reach the player's picture

    session.simulation.submitCommand(
        BoardCommand{player, trackOf(session, powered)}); // it fights back: refused
    session.simulation.submitCommand(BoardCommand{player, trackOf(session, victim)});
    session.simulation.runFor(SimDuration::milliseconds(100));
    GX_EXPECT(world.isAlive(powered));
    GX_EXPECT(!world.isAlive(victim));
    GX_EXPECT(session.journalContains("resiste"));
    GX_EXPECT_EQ(world.components<CargoHold>().get(player).amount(content::kGoodWater), 20u); // hold is 20 t
    GX_EXPECT_EQ(session.sandbox.stats().cargoLost[content::kGoodWater], 30u); // the rest is lost
    GX_EXPECT_NEAR(session.sandbox.reputation(), content::kReputationBoarding, 1e-6);

    // A second boarding makes the player hostile: the port refuses to trade.
    session.simulation.submitCommand(BoardCommand{player, trackOf(session, second)});
    session.simulation.runFor(SimDuration::milliseconds(100));
    GX_EXPECT(session.sandbox.hostile());
    session.simulation.submitCommand(TradeCommand{player, content::kGoodWater, -1});
    session.simulation.runFor(SimDuration::milliseconds(100));
    GX_EXPECT(session.journalContains("se niega a comerciar"));
    GX_EXPECT_EQ(world.components<CargoHold>().get(player).amount(content::kGoodWater), 20u);
    GX_EXPECT_EQ(session.sandbox.stats().boardings, 2u);
}

GX_TEST(Sandbox, TheAuthorityBuysPatrolsItCanAfford) {
    SandboxConfig config;
    config.haulers = 0; // no trade, no taxes: the treasury only drains
    config.pirates = 0;
    Session session(0, config);
    session.sandbox.populate(session.simulation);
    const World& world = session.simulation.world();
    session.simulation.runFor(SimDuration::minutes(3)); // first budget review at 2 min
    GX_EXPECT_EQ(world.components<PatrolBrain>().size(), 1u);
    GX_EXPECT_EQ(session.sandbox.treasury(), content::kStartingTreasury - content::kPatrolCommissionCost);
    GX_EXPECT(session.journalContains("pone en servicio"));
    const EntityId patrol = world.components<PatrolBrain>().entities()[0];
    GX_EXPECT_EQ(world.components<ShipIdentity>().get(patrol).faction,
                 static_cast<u32>(content::kFactionAuthority));
    GX_EXPECT(world.components<SensorSuite>().get(patrol).activeOn); // overt

    // 14,000 cr cannot buy a second patrol plus its reserve; upkeep drains the rest in ~9.3 h.
    session.simulation.runFor(SimDuration::hours(10));
    GX_EXPECT_EQ(session.sandbox.stats().patrolsCommissioned, 1u);
    GX_EXPECT_EQ(session.sandbox.stats().patrolsDecommissioned, 1u);
    GX_EXPECT_EQ(world.components<PatrolBrain>().size(), 0u);
    GX_EXPECT(session.journalContains("falta de fondos"));
}

GX_TEST(Sandbox, PatrolsDestroyRaiders) {
    SandboxConfig config;
    config.haulers = 0;
    config.pirates = 1;
    Session session(0, config);
    session.sandbox.populate(session.simulation);
    World& world = session.simulation.world();
    session.simulation.runFor(SimDuration::minutes(3));
    GX_REQUIRE(world.components<PatrolBrain>().size() == 1u);
    const EntityId patrol = world.components<PatrolBrain>().entities()[0];
    const EntityId pirate = world.components<PirateBrain>().entities()[0];
    const Vec3d spot{0.0, 0.0, 5e10};
    world.components<Kinematics>().get(patrol) = {spot, {}, {}};
    world.components<ShipControl>().get(patrol) = {};
    world.components<Kinematics>().get(pirate) = {spot + Vec3d{150'000.0, 0.0, 0.0}, {}, {}};
    world.components<ShipControl>().get(pirate).point = spot + Vec3d{150'000.0, 0.0, 0.0};
    world.components<ShipModules>().get(pirate).modules[0].health = 5.0;
    session.simulation.runFor(SimDuration::seconds(30));
    GX_EXPECT(!world.isAlive(pirate));
    GX_EXPECT_EQ(session.sandbox.stats().piratesKilledByPatrols, 1u);
    GX_EXPECT_EQ(session.sandbox.stats().bountiesPaid, 0); // bounties are for the player
}

GX_TEST(Sandbox, PatrolsHuntAHostilePlayer) {
    Session session(0, peaceful());
    session.sandbox.populate(session.simulation);
    World& world = session.simulation.world();
    const EntityId player = session.sandbox.playerShip();
    session.simulation.runFor(SimDuration::minutes(3)); // a patrol is in service
    GX_REQUIRE(world.components<PatrolBrain>().size() == 1u);
    parkHaulerNextToPlayer(session, 0, true, 0);
    parkHaulerNextToPlayer(session, 1, true, 0);
    session.simulation.runFor(SimDuration::milliseconds(1'100));
    for (usize i = 0; i < 2; ++i) {
        session.simulation.submitCommand(
            BoardCommand{player, trackOf(session, world.components<HaulerBrain>().entities()[i])});
        session.simulation.runFor(SimDuration::milliseconds(100));
    }
    GX_REQUIRE(session.sandbox.hostile());
    // Bring the patrol within sight: it identifies the player (transponder on) and goes after it.
    const EntityId patrol = world.components<PatrolBrain>().entities()[0];
    const Kinematics& mine = world.components<Kinematics>().get(player);
    world.components<Kinematics>().get(patrol) = {
        mine.position + Vec3d{300'000.0, 0.0, 0.0}, mine.velocity, {}};
    world.components<ShipControl>().get(patrol) = {};
    session.simulation.runFor(SimDuration::seconds(5));
    GX_EXPECT(world.components<PatrolBrain>().get(patrol).state == PatrolState::Engaging);
    GX_EXPECT(world.components<CombatControl>().get(patrol).targetTrack != 0);
    GX_EXPECT_EQ(session.sandbox.stats().wantedChases, 1u);
    GX_EXPECT(session.journalContains("te busca"));

    // Caught: the player's ship is destroyed, and with it the debt (no spawn-camping at the station).
    world.components<ShipModules>().get(player).modules[0].health = 5.0;
    session.simulation.runFor(SimDuration::seconds(30));
    GX_EXPECT(!world.isAlive(player));
    GX_EXPECT(!session.sandbox.hostile());
    GX_EXPECT_NEAR(session.sandbox.reputation(), content::kReputationAfterDeath, 0.5);
    GX_EXPECT(session.journalContains("saldada"));
}

GX_TEST(Sandbox, PatrolsAnsweringDistressCallsSaveTraders) {
    // Causality: the treasury's patrols answer the traders' calls for help, and fewer traders die.
    const auto lost = [](u32 patrols) {
        SandboxConfig config;
        config.maxPatrols = patrols;
        Session session(3, config);
        session.sandbox.populate(session.simulation);
        session.simulation.runFor(SimDuration::hours(8));
        GX_EXPECT(session.sandbox.stats().distressCalls > 0);
        GX_EXPECT(patrols == 0 || session.sandbox.stats().distressAnswered > 0);
        return session.sandbox.stats().haulersLost;
    };
    const u64 unprotected = lost(0);
    const u64 protectedByPatrols = lost(3);
    GX_EXPECT(protectedByPatrols < unprotected);
}
