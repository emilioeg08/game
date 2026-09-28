// Headless simulation runner: simulates the synthetic galaxy (or, with --sandbox, the playable star system)
// without any renderer or UI.
// Unpaced by default (as fast as possible); --speed paces it against the wall clock like interactive play.
// Also the first save tool: --save/--load continue a campaign, --inspect describes a save file, and
// --record/--replay verify that a recorded command sequence reproduces the same final state.

#include "Engine/Core/Log.h"
#include "Engine/Core/Platform.h"
#include "Engine/Core/Random.h"
#include "Engine/Jobs/JobSystem.h"
#include "Engine/Profiling/MemoryUsage.h"
#include "Engine/Profiling/Profiler.h"
#include "Engine/Serialization/Binary.h"
#include "Engine/Serialization/SaveFile.h"
#include "Engine/Text/Localization.h"
#include "Engine/Time/Stopwatch.h"
#include "Engine/Time/TimeController.h"
#include "Game/Sandbox/Content.h"
#include "Game/Sandbox/Sandbox.h"
#include "Scenarios/SyntheticGalaxy.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Bodies/CelestialBody.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace gx;

namespace {

constexpr std::string_view kChannel = "Headless";
constexpr u32 kReplayChunk = fourCC("RPLY");

struct Options {
    u64 seed = 1;
    u32 systems = 1000;
    u32 bodies = 16;
    u32 goods = 8;
    u32 days = 30;
    u32 threads = platform::hardwareThreadCount();
    u32 speed = 0; // 0: unpaced
    bool raids = false;
    std::string tracePath;
    std::string savePath;
    std::string loadPath;
    std::string inspectPath;
    std::string recordPath;
    std::string replayPath;
    bool profile = true;
    LogLevel logLevel = LogLevel::Info;
    // --sandbox: the playable slice (star system, haulers, pirates) instead of the synthetic galaxy.
    bool sandbox = false;
    u32 minutes = 60;
    u32 haulers = 12;
    u32 pirates = 3;
    u32 patrols = 3;
    bool traderContracts = true;
    bool finance = true;
    std::string language; // journal language: a catalog in data/lang (default: Spanish, the source)
    u32 maxHaulers = 0;
    i32 flyTo = -1;       // order the player's ship to this port at the start
    bool journal = false; // print the game journal at the end
    bool markets = false; // print every port's market at the end
    bool dump = false;    // print every hauler and patrol at the end
    // The player's company (ADR-037): ships bought at the start, with standing orders.
    u32 miners = 0;         // mine the rock fields
    u32 iceMiners = 0;      // mine the ice field
    u32 fleetHaulers = 0;   // trade on their own
    u32 escorts = 0;        // escort the player's ship
    i64 companyCredits = 0; // added to the starting account
};

void printUsage() {
    std::printf("Usage: gx_headless [options]\n"
                "  --seed <n>          world seed (default 1)\n"
                "  --systems <n>       star systems (default 1000)\n"
                "  --bodies <n>        bodies per star system (default 16)\n"
                "  --goods <n>         goods per star system (default 8)\n"
                "  --days <n>          simulated days (default 30)\n"
                "  --threads <n>       total threads including the main thread (default: hardware threads)\n"
                "  --speed <x>         pace against the wall clock at x simulated seconds per real second\n"
                "                      (default: unpaced, as fast as possible)\n"
                "  --raids             submit a RaidConvoys command every simulated day (external input)\n"
                "  --save <file>       save the simulation at the end of the run\n"
                "  --load <file>       continue from a save (same --systems/--bodies/--goods)\n"
                "  --inspect <file>    describe a save file and exit\n"
                "  --record <file>     record the applied commands and the final state hash\n"
                "  --replay <file>     re-run a recording and verify the final state hash\n"
                "  --trace <file>      write a Chrome/Perfetto trace (https://ui.perfetto.dev)\n"
                "  --no-profile        disable profiling zones\n"
                "  --sandbox           run the playable star system instead (with --seed, --minutes,\n"
                "                      --haulers, --pirates): combat and traffic statistics\n"
                "  --patrols <n>       sandbox: most patrols the Authority may keep (default 3)\n"
                "  --no-trader-contracts  sandbox: only the player takes supply contracts\n"
                "  --no-finance        sandbox: losses replaced for free, no bank or insurance (M3.4)\n"
                "  --max-haulers <n>   sandbox: most traders the business can grow to (default 2x)\n"
                "  --fly-to <i>        sandbox: send the player's ship to port i at the start\n"
                "  --journal           sandbox: print the game journal at the end\n"
                "  --lang <code>       sandbox: journal language, a catalog in data/lang (e.g. en)\n"
                "  --markets           sandbox: print every port's market at the end\n"
                "  --dump              sandbox: print every hauler and patrol at the end\n"
                "  --miners <n>        sandbox: the player's company buys n Mineros for the rock fields\n"
                "  --ice-miners <n>    sandbox: ... and n for the ice field\n"
                "  --fleet-haulers <n> sandbox: ... n Cargueros that trade on their own\n"
                "  --escorts <n>       sandbox: ... n Escoltas for the player's ship\n"
                "  --company-credits <n>  sandbox: added to the company's starting account (default 0;\n"
                "                      ships it cannot pay for are financed with the least down payment)\n"
                "  --log-level <lvl>   trace|debug|info|warn|error (default info)\n");
}

template <typename T>
bool parseNumber(std::string_view text, T& out) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size();
}

// Returns an exit code if the program should stop, -1 to continue.
int parseOptions(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto value = [&]() -> std::string_view {
            return i + 1 < argc ? std::string_view(argv[++i]) : "";
        };
        const auto path = [&](std::string& out) {
            out = std::string(value());
            return !out.empty();
        };
        bool ok = true;
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--seed") {
            ok = parseNumber(value(), options.seed);
        } else if (arg == "--systems") {
            ok = parseNumber(value(), options.systems) && options.systems > 0;
        } else if (arg == "--bodies") {
            ok = parseNumber(value(), options.bodies) && options.bodies > 0;
        } else if (arg == "--goods") {
            ok = parseNumber(value(), options.goods) && options.goods > 0;
        } else if (arg == "--days") {
            ok = parseNumber(value(), options.days) && options.days > 0;
        } else if (arg == "--threads") {
            ok = parseNumber(value(), options.threads) && options.threads > 0;
        } else if (arg == "--speed") {
            ok = parseNumber(value(), options.speed) && options.speed > 0;
        } else if (arg == "--raids") {
            options.raids = true;
        } else if (arg == "--save") {
            ok = path(options.savePath);
        } else if (arg == "--load") {
            ok = path(options.loadPath);
        } else if (arg == "--inspect") {
            ok = path(options.inspectPath);
        } else if (arg == "--record") {
            ok = path(options.recordPath);
        } else if (arg == "--replay") {
            ok = path(options.replayPath);
        } else if (arg == "--trace") {
            ok = path(options.tracePath);
        } else if (arg == "--sandbox") {
            options.sandbox = true;
        } else if (arg == "--minutes") {
            ok = parseNumber(value(), options.minutes) && options.minutes > 0;
        } else if (arg == "--haulers") {
            ok = parseNumber(value(), options.haulers);
        } else if (arg == "--pirates") {
            ok = parseNumber(value(), options.pirates);
        } else if (arg == "--no-trader-contracts") {
            options.traderContracts = false;
        } else if (arg == "--lang") {
            options.language = std::string(value());
            ok = !options.language.empty();
        } else if (arg == "--no-finance") {
            options.finance = false;
        } else if (arg == "--max-haulers") {
            ok = parseNumber(value(), options.maxHaulers);
        } else if (arg == "--patrols") {
            ok = parseNumber(value(), options.patrols);
        } else if (arg == "--fly-to") {
            ok = parseNumber(value(), options.flyTo) && options.flyTo >= 0;
        } else if (arg == "--journal") {
            options.journal = true;
        } else if (arg == "--markets") {
            options.markets = true;
        } else if (arg == "--dump") {
            options.dump = true;
        } else if (arg == "--miners") {
            ok = parseNumber(value(), options.miners);
        } else if (arg == "--ice-miners") {
            ok = parseNumber(value(), options.iceMiners);
        } else if (arg == "--fleet-haulers") {
            ok = parseNumber(value(), options.fleetHaulers);
        } else if (arg == "--escorts") {
            ok = parseNumber(value(), options.escorts);
        } else if (arg == "--company-credits") {
            ok = parseNumber(value(), options.companyCredits) && options.companyCredits >= 0;
        } else if (arg == "--no-profile") {
            options.profile = false;
        } else if (arg == "--log-level") {
            ok = parseLogLevel(value(), options.logLevel);
        } else {
            ok = false;
        }
        if (!ok) {
            std::fprintf(stderr, "invalid or incomplete argument: %.*s\n\n", static_cast<int>(arg.size()),
                         arg.data());
            printUsage();
            return 2;
        }
    }
    if (!options.recordPath.empty() && (!options.loadPath.empty() || !options.replayPath.empty())) {
        std::fprintf(
            stderr,
            "--record starts from a generated galaxy: it cannot be combined with --load or --replay\n");
        return 2;
    }
    return -1;
}

int inspectSave(const std::string& path) {
    SaveFileContents save;
    std::string error;
    if (!readSaveFile(path, save, error)) {
        std::fprintf(stderr, "cannot read %s: %s\n", path.c_str(), error.c_str());
        return 1;
    }
    std::printf("file            : %s\n", path.c_str());
    std::printf("format version  : %u\n", save.info.formatVersion);
    std::printf("engine version  : %s\n", save.info.engineVersion.c_str());
    std::printf("description     : %s\n", save.info.description.c_str());
    std::printf("payload         : %llu bytes, hash %016llx (verified)\n",
                static_cast<unsigned long long>(save.info.payloadBytes),
                static_cast<unsigned long long>(save.info.payloadHash));
    std::printf("chunks          :\n");
    for (const ChunkInfo& chunk : listChunks(save.payload)) {
        std::printf("  %s v%u  %12llu bytes at offset %llu\n", fourCCToString(chunk.tag).c_str(),
                    chunk.version, static_cast<unsigned long long>(chunk.size),
                    static_cast<unsigned long long>(chunk.offset));
    }
    return 0;
}

// A recording: the parameters that generate the initial state, the applied commands and the final hash.
struct Recording {
    u64 seed = 0;
    u32 systems = 0;
    u32 bodies = 0;
    u32 goods = 0;
    u32 days = 0;
    std::vector<CommandRecord> commands;
    u64 finalHash = 0;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io(seed);
        ar.io(systems);
        ar.io(bodies);
        ar.io(goods);
        ar.io(days);
        ar.io(commands);
        ar.io(finalHash);
    }
};

bool readRecording(const std::string& path, Recording& recording, std::string& error) {
    SaveFileContents file;
    if (!readSaveFile(path, file, error)) {
        return false;
    }
    BinaryReader reader(file.payload);
    BinaryReader::Chunk chunk;
    if (reader.beginChunk(kReplayChunk, chunk)) {
        reader.io(recording);
        reader.endChunk(chunk);
    }
    if (!reader.ok()) {
        error = reader.error();
        return false;
    }
    return true;
}

void logProgress(const Simulation& simulation, const SyntheticGalaxy& galaxy, const Stopwatch& wall,
                 SimTime runStart) {
    const f64 simulatedDays = (simulation.now() - runStart).toDays();
    GX_LOG_INFO(kChannel, "{} | steps {:>9} | convoys {:>6} | output {:.4e} | {:.1f} sim-days/s",
                formatSimTime(simulation.now()), simulation.stepCount(), galaxy.activeConvoys(),
                galaxy.galacticOutput(), simulatedDays / std::max(wall.elapsedSeconds(), 1e-9));
}

void runUnpaced(Simulation& simulation, const SyntheticGalaxy& galaxy, SimTime end, u32 days,
                const Stopwatch& wall) {
    const SimTime runStart = simulation.now();
    const u32 reportEveryDays = std::max(1u, days / 10);
    while (simulation.now() < end) {
        simulation.runUntil(std::min(end, simulation.now() + SimDuration::days(reportEveryDays)));
        logProgress(simulation, galaxy, wall, runStart);
    }
}

void runPaced(Simulation& simulation, const SyntheticGalaxy& galaxy, SimTime end, u32 speed,
              const Stopwatch& wall) {
    constexpr u64 kFrameBudgetNs = 10'000'000; // keep a ~60 Hz frame responsive
    const SimTime runStart = simulation.now();
    TimeController controller(simulation.now());
    controller.setSpeed(speed);
    const SimDuration reportInterval = SimDuration::seconds(std::max<i64>(1, speed)); // ~once per real second
    SimTime nextReport = simulation.now() + reportInterval;
    u64 lastNs = platform::monotonicNanoseconds();

    while (simulation.now() < end) {
        const u64 nowNs = platform::monotonicNanoseconds();
        const SimTime target = std::min(end, controller.update(nowNs - lastNs, simulation.now()));
        lastNs = nowNs;
        simulation.runUntil(target, kFrameBudgetNs);
        if (simulation.now() >= nextReport) {
            logProgress(simulation, galaxy, wall, runStart);
            nextReport = simulation.now() + reportInterval;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // stand-in for rendering and UI work
    }
    if (controller.droppedTime() > SimDuration{}) {
        GX_LOG_WARN(kChannel, "could not keep up with {}x: dropped {} of simulated time", speed,
                    formatDuration(controller.droppedTime()));
    }
}

// Economy summary: flows per good, price spread between ports, conservation of goods and trader activity.
void printEconomy(const Simulation& simulation, const Sandbox& sandbox, bool perPort) {
    const World& world = simulation.world();
    const std::vector<GoodDef>& goods = sandbox.economy().goods();
    const ComponentStore<Market>& markets = world.components<Market>();
    const ComponentStore<CargoHold>& holds = world.components<CargoHold>();
    const SandboxStats& stats = sandbox.stats();
    std::printf("economy: %-12s %9s %9s %7s %9s %7s %9s %7s %7s %7s  %s\n", "good", "produced", "consumed",
                "mined", "shortage", "lost", "stock", "cargo", "min cr", "max cr", "balance");
    for (GoodId g = 0; g < goods.size(); ++g) {
        f64 produced = 0.0;
        f64 consumed = 0.0;
        f64 shortage = 0.0;
        f64 stock = 0.0;
        f64 minPrice = 1e18;
        f64 maxPrice = 0.0;
        for (const Market& market : markets.values()) {
            if (const MarketGood* good = market.find(g)) {
                produced += good->produced;
                consumed += good->consumed;
                shortage += good->shortage;
                stock += good->stock;
                const f64 price = unitPrice(goods[g].basePrice, good->stock, good->target);
                minPrice = std::min(minPrice, price);
                maxPrice = std::max(maxPrice, price);
            }
        }
        f64 cargo = 0.0;
        for (const CargoHold& hold : holds.values()) {
            cargo += hold.amount(g);
        }
        f64 mined = 0.0;
        for (const Deposit& deposit : world.components<Deposit>().values()) {
            mined += deposit.good == g ? deposit.extracted : 0.0;
        }
        const f64 lost = g < stats.cargoLost.size() ? static_cast<f64>(stats.cargoLost[g]) : 0.0;
        const f64 initial = g < sandbox.initialStock().size() ? sandbox.initialStock()[g] : 0.0;
        // initial + produced + mined - consumed - lost must equal what is in the markets and in the holds.
        const f64 balance = initial + produced + mined - consumed - lost - stock - cargo;
        std::printf("economy: %-12s %9.0f %9.0f %7.0f %9.0f %7.0f %9.0f %7.0f %7.0f %7.0f  %+.3f\n",
                    goods[g].name.c_str(), produced, consumed, mined, shortage, lost, stock, cargo, minPrice,
                    maxPrice, balance);
    }
    i64 haulerCredits = 0;
    for (const EntityId hauler : world.components<HaulerBrain>().entities()) {
        if (const Wallet* wallet = world.components<Wallet>().tryGet(hauler)) {
            haulerCredits += wallet->credits;
        }
    }
    std::printf(
        "traders: %llu loads, %llu t delivered, %llu repositioning and %llu exploring trips, credits on hand "
        "%lld (%zu haulers)\n",
        static_cast<unsigned long long>(stats.haulerTrades),
        static_cast<unsigned long long>(stats.tonnesDelivered),
        static_cast<unsigned long long>(stats.repositionTrips),
        static_cast<unsigned long long>(stats.explorationTrips), static_cast<long long>(haulerCredits),
        world.components<HaulerBrain>().size());
    std::printf(
        "money: treasury %lld | taxes %lld, repairs %lld, bounties %lld | wages %lld | bankruptcies %llu, "
        "boardings %llu\n",
        static_cast<long long>(sandbox.treasury()), static_cast<long long>(stats.taxesCollected),
        static_cast<long long>(stats.repairFees), static_cast<long long>(stats.bountiesPaid),
        static_cast<long long>(stats.wagesPaid), static_cast<unsigned long long>(stats.bankruptcies),
        static_cast<unsigned long long>(stats.boardings));
    std::printf(
        "patrols: %zu in service | %llu commissioned, %llu decommissioned, %llu lost | %llu pirates killed "
        "| %llu distress calls, %llu answered | upkeep %lld\n",
        world.components<PatrolBrain>().size(), static_cast<unsigned long long>(stats.patrolsCommissioned),
        static_cast<unsigned long long>(stats.patrolsDecommissioned),
        static_cast<unsigned long long>(stats.patrolsLost),
        static_cast<unsigned long long>(stats.piratesKilledByPatrols),
        static_cast<unsigned long long>(stats.distressCalls),
        static_cast<unsigned long long>(stats.distressAnswered),
        static_cast<long long>(stats.patrolUpkeepPaid));
    std::printf(
        "contracts: %llu posted | %llu completed (%llu by traders), %llu failed, %llu expired, %llu "
        "cancelled | "
        "%zu on the board\n",
        static_cast<unsigned long long>(stats.contractsPosted),
        static_cast<unsigned long long>(stats.contractsCompleted),
        static_cast<unsigned long long>(stats.contractsCompletedByTraders),
        static_cast<unsigned long long>(stats.contractsFailed),
        static_cast<unsigned long long>(stats.contractsExpired),
        static_cast<unsigned long long>(stats.contractsCancelled),
        static_cast<usize>(std::count_if(sandbox.contracts().begin(), sandbox.contracts().end(),
                                         [](const Contract& c) { return c.state == ContractState::Open; })));
    if (sandbox.config().finance) {
        const BankLedger& bank = sandbox.bank();
        const MutualLedger& mutual = sandbox.mutual();
        std::printf(
            "bank: equity %lld (cash %lld, loans %lld, deposits %lld) | interest %lld earned, %lld paid "
            "| %llu loans, %llu defaults, %lld written off\n",
            static_cast<long long>(bank.equity()), static_cast<long long>(bank.cash),
            static_cast<long long>(bank.loans), static_cast<long long>(bank.deposits),
            static_cast<long long>(bank.interestEarned), static_cast<long long>(bank.interestPaid),
            static_cast<unsigned long long>(bank.loansGranted),
            static_cast<unsigned long long>(bank.defaults), static_cast<long long>(bank.writtenOff));
        std::printf(
            "mutual: fund %lld | premiums %lld, claims %lld (repairs %lld, %llu hulls, %llu short) | "
            "premium %.0f cr/h, %.3f losses per ship-hour | a new trader expected to earn %.0f cr/h\n",
            static_cast<long long>(mutual.fund), static_cast<long long>(mutual.premiums),
            static_cast<long long>(mutual.claimsPaid), static_cast<long long>(mutual.repairsPaid),
            static_cast<unsigned long long>(mutual.claims),
            static_cast<unsigned long long>(mutual.claimsShort), sandbox.premiumPerHour(),
            mutual.losses.estimate(content::kLossRatePrior, content::kLossRatePriorHours),
            sandbox.expectedEarningsPerHour());
        std::printf(
            "fleet: %zu haulers (peak %llu) | %llu ships bought: %llu by returning owners, %llu by "
            "expansion, %llu by newcomers | %llu repossessed, %llu owners left, %zu waiting | capital "
            "in %lld, out %lld\n",
            world.components<HaulerBrain>().size(), static_cast<unsigned long long>(stats.peakHaulers),
            static_cast<unsigned long long>(stats.shipsBought),
            static_cast<unsigned long long>(stats.shipsByReturningOwners),
            static_cast<unsigned long long>(stats.shipsByExpansion),
            static_cast<unsigned long long>(stats.shipsByOutsiders),
            static_cast<unsigned long long>(stats.repossessions),
            static_cast<unsigned long long>(stats.ownersRetired), sandbox.buyers().size(),
            static_cast<long long>(stats.capitalIn), static_cast<long long>(stats.capitalOut));
    }
    if (!perPort) {
        return;
    }
    for (usize i = 0; i < markets.size(); ++i) {
        const EntityId port = markets.entities()[i];
        std::printf("  %-22s danger %.2f\n", world.components<CelestialBody>().get(port).name.c_str(),
                    sandbox.danger(port, simulation.now()));
        for (const MarketGood& good : markets.values()[i].goods) {
            std::printf(
                "      %-12s stock %7.0f / %7.0f  price %6.0f  prod %5.0f/h  use %5.0f/h  shortage %7.0f\n",
                goods[good.good].name.c_str(), good.stock, good.target,
                unitPrice(goods[good.good].basePrice, good.stock, good.target),
                productionRate(markets.values()[i], good.good),
                consumptionRate(markets.values()[i], good.good), good.shortage);
        }
    }
}

// The playable slice, unpaced: traffic and combat statistics every tenth of the run (balance and cost).
int runSandbox(const Options& options) {
    JobSystem jobs(options.threads - 1);
    SandboxConfig config;
    config.seed = options.seed;
    config.haulers = options.haulers;
    config.pirates = options.pirates;
    config.maxPatrols = options.patrols;
    config.tradersTakeContracts = options.traderContracts;
    config.finance = options.finance;
    config.maxHaulers = options.maxHaulers;
    Sandbox sandbox(config);
    Simulation simulation(Simulation::Config{.seed = config.seed}, jobs);
    sandbox.install(simulation);
    sandbox.populate(simulation);
    // Who hits and who destroys whom, by faction (observation only: not part of the simulation state).
    constexpr u32 kFactions = content::kFactionCount;
    std::array<std::array<u64, kFactions + 1>, kFactions + 1> hits{};
    std::array<std::array<u64, kFactions + 1>, kFactions + 1> kills{};
    const auto factionOf = [&](const World& world, EntityId ship) -> u32 {
        const ShipIdentity* identity =
            world.isAlive(ship) ? world.components<ShipIdentity>().tryGet(ship) : nullptr;
        return identity != nullptr ? identity->faction : kFactions; // kFactions: unknown (shooter gone)
    };
    simulation.events().channel<ShipDamaged>().subscribe([&](const ShipDamaged& e, const TickContext& c) {
        ++hits[factionOf(c.world, e.attacker)][factionOf(c.world, e.ship)];
    });
    simulation.events().channel<ShipDestroyed>().subscribe([&](const ShipDestroyed& e, const TickContext& c) {
        ++kills[factionOf(c.world, e.attacker)][factionOf(c.world, e.ship)];
    });
    GX_LOG_INFO(kChannel, "sandbox | system {} | seed {} | {} haulers, {} pirates | {} minutes",
                sandbox.systemName(), config.seed, config.haulers, config.pirates, options.minutes);
    if (options.flyTo >= 0 && static_cast<usize>(options.flyTo) < sandbox.ports().size()) {
        simulation.submitCommand(PilotCommand{sandbox.playerShip(),
                                              FlightMode::Approach,
                                              sandbox.ports()[static_cast<usize>(options.flyTo)],
                                              {},
                                              {}});
    }

    // The player's company: ships bought at the home station's yards (cash if the account allows, else the
    // least down payment the bank accepts), then their standing orders.
    sandbox.account().credits += options.companyCredits;
    const auto buy = [&](u32 shipClass) -> EntityId {
        const World& world = simulation.world();
        const i64 price = content::kShipPrices[shipClass];
        const i64 account = sandbox.company().account.credits;
        i64 down = price;
        if (account < price && config.finance) {
            const auto room = static_cast<i64>((1.0 - content::kMinDownPayment) *
                                               static_cast<f64>(sandbox.fleetValue(world) + price)) -
                              sandbox.company().debt;
            down = std::max<i64>(price - room, 0);
        }
        const usize before = world.components<FleetBrain>().size();
        simulation.submitCommand(BuyShipCommand{sandbox.playerShip(), shipClass, down, true});
        simulation.runFor(SimDuration::seconds(1));
        const ComponentStore<FleetBrain>& brains = simulation.world().components<FleetBrain>();
        if (brains.size() == before) {
            GX_LOG_WARN(kChannel, "the company could not buy a {} ({} cr down, {} cr in the account)",
                        content::kShipClasses[shipClass].name, down, account);
            return {};
        }
        return brains.entities().back();
    };
    std::vector<EntityId> rockFields;
    std::vector<EntityId> iceFields;
    for (const EntityId field : sandbox.fields()) {
        (simulation.world().components<CelestialBody>().get(field).kind == BodyKind::IceField ? iceFields
                                                                                              : rockFields)
            .push_back(field);
    }
    const auto order = [&](EntityId ship, FleetOrder what, EntityId site) {
        if (ship.isValid()) {
            simulation.submitCommand(FleetOrderCommand{ship, what, site, {}});
        }
    };
    // All the miners work the rock field nearest to the home station: one escort can cover them all.
    if (!rockFields.empty()) {
        const Vec3d home = bodyStateAt(simulation.world(), sandbox.homePort(), simulation.now()).position;
        std::sort(rockFields.begin(), rockFields.end(), [&](EntityId a, EntityId b) {
            return length(bodyStateAt(simulation.world(), a, simulation.now()).position - home) <
                   length(bodyStateAt(simulation.world(), b, simulation.now()).position - home);
        });
    }
    for (u32 i = 0; i < options.miners && !rockFields.empty(); ++i) {
        order(buy(content::kShipClassMiner), FleetOrder::Mine, rockFields.front());
    }
    for (u32 i = 0; i < options.iceMiners && !iceFields.empty(); ++i) {
        order(buy(content::kShipClassMiner), FleetOrder::Mine, iceFields[i % iceFields.size()]);
    }
    for (u32 i = 0; i < options.fleetHaulers; ++i) {
        order(buy(content::kShipClassHauler), FleetOrder::Trade, {});
    }
    // Escorts guard the miners first (one each, in order), then the player's ship.
    std::vector<EntityId> wards;
    for (const EntityId ship : simulation.world().components<FleetBrain>().entities()) {
        if (simulation.world().components<ShipIdentity>().get(ship).shipClass == content::kShipClassMiner) {
            wards.push_back(ship);
        }
    }
    for (u32 i = 0; i < options.escorts; ++i) {
        order(buy(content::kShipClassEscort), FleetOrder::Escort, i < wards.size() ? wards[i] : EntityId{});
    }
    const bool company = options.miners + options.iceMiners + options.fleetHaulers + options.escorts > 0;
    // Where the ore goes: the bid of every market that uses it (price and stock against its target).
    const auto oreBids = [&] {
        std::string out;
        const ComponentStore<Market>& markets = simulation.world().components<Market>();
        for (usize i = 0; i < markets.size(); ++i) {
            const MarketGood* ore = markets.values()[i].find(content::kGoodOre);
            if (ore != nullptr && consumptionRate(markets.values()[i], content::kGoodOre) > 0.0) {
                out += std::format(
                    " {} {} cr/t (x{:.2f})",
                    simulation.world().components<CelestialBody>().get(markets.entities()[i]).name,
                    sellPrice(*ore, sandbox.economy().basePrice(content::kGoodOre)),
                    ore->target > 0.0 ? ore->stock / ore->target : 0.0);
            }
        }
        return out;
    };

    const Stopwatch wall;
    const SimTime end = simulation.now() + SimDuration::minutes(options.minutes);
    const SimDuration report = SimDuration::minutes(std::max<u32>(1, options.minutes / 10));
    while (simulation.now() < end) {
        simulation.runUntil(std::min(end, simulation.now() + report));
        const SandboxStats& stats = sandbox.stats();
        const CombatStats& combat = sandbox.combat().stats();
        const World& world = simulation.world();
        GX_LOG_INFO(
            kChannel,
            "{} | haulers {:>3} | pirates {:>2} | hunts {:>3} | shots {:>6} hits {:>6} | lost: haulers {:>2} "
            "pirates {:>2} fled {:>2} player {} | x{:.0f}",
            formatSimTime(simulation.now(), content::kEpochYear), world.components<HaulerBrain>().size(),
            world.components<PirateBrain>().size(), stats.hunts, combat.shotsFired, combat.hits,
            stats.haulersLost, stats.piratesLost, stats.piratesLeft, stats.playerDeaths,
            (simulation.now() - SimTime::epoch()).toSeconds() / std::max(wall.elapsedSeconds(), 1e-9));
        GX_LOG_INFO(kChannel, "    trade | delivered {:>6} t | loads {:>4} | bankruptcies {:>3}",
                    stats.tonnesDelivered, stats.haulerTrades, stats.bankruptcies);
        if (config.finance) {
            GX_LOG_INFO(
                kChannel,
                "    finance | delivered {:>6} t | earnings {:>5.0f} cr/h, premium {:>4.0f} cr/h | bank "
                "cash {:>6} loans {:>6} deposits {:>6} | mutual {:>6}",
                stats.tonnesDelivered, sandbox.expectedEarningsPerHour(), sandbox.premiumPerHour(),
                sandbox.bank().cash, sandbox.bank().loans, sandbox.bank().deposits, sandbox.mutual().fund);
        }
        if (company) {
            const CompanyBooks& books = sandbox.company();
            const CompanyTotals& t = books.totals;
            GX_LOG_INFO(
                kChannel,
                "    company | account {:>7} debt {:>6} | {} ships worth {:>6} | net worth {:>7} | mined "
                "{:>5} t | sales {:>7} | wages {:>6} premiums {:>6} interest {:>5} | lost {} | premium "
                "{:.0f} cr/h per Carguero",
                books.account.credits, books.debt, world.components<FleetBrain>().size(),
                sandbox.fleetValue(world), sandbox.companyWorth(world), t.tonnesMined, t.sales, t.wages,
                t.premiums, t.interest, t.shipsLost,
                sandbox.companyPremiumPerHour(content::kHaulerHullPrice));
            GX_LOG_INFO(kChannel, "    ore bids |{}", oreBids());
        }
    }
    std::printf("ore:%s\n", oreBids().c_str());
    if (company) {
        const World& world = simulation.world();
        const ComponentStore<FleetBrain>& brains = world.components<FleetBrain>();
        std::printf("company fleet (income - expenses since bought):\n");
        for (usize i = 0; i < brains.size(); ++i) {
            const EntityId ship = brains.entities()[i];
            const OwnedShip* owned = world.components<OwnedShip>().tryGet(ship);
            const CargoHold& hold = world.components<CargoHold>().get(ship);
            std::printf(
                "  %-14s %-9s %-7s %-10s trips %3u | income %7lld expenses %7lld net %7lld | cargo %3u t\n",
                world.components<ShipIdentity>().get(ship).name.c_str(),
                content::kShipClasses[world.components<ShipIdentity>().get(ship).shipClass].name,
                toString(brains.values()[i].order), toString(brains.values()[i].task),
                brains.values()[i].trips, static_cast<long long>(owned != nullptr ? owned->income : 0),
                static_cast<long long>(owned != nullptr ? owned->expenses : 0),
                static_cast<long long>(owned != nullptr ? owned->income - owned->expenses : 0), hold.used());
        }
        for (const EntityId field : sandbox.fields()) {
            const Deposit& deposit = world.components<Deposit>().get(field);
            std::printf("  field %-24s %-6s reserve %6.0f / %6.0f t | extracted %7.0f t\n",
                        world.components<CelestialBody>().get(field).name.c_str(),
                        content::goodTable()[deposit.good].name.c_str(), deposit.reserve, deposit.size,
                        deposit.extracted);
        }
    }
    const World& world = simulation.world();
    const ComponentStore<PirateBrain>& pirates = world.components<PirateBrain>();
    for (usize i = 0; i < pirates.size(); ++i) {
        const EntityId ship = pirates.entities()[i];
        const ShipControl& control = world.components<ShipControl>().get(ship);
        const ShipModules& modules = world.components<ShipModules>().get(ship);
        const Kinematics& state = world.components<Kinematics>().get(ship);
        std::printf(
            "  pirate %-12s %-8s mode %-8s phase %-10s arrived %d track %u fire %u structure %.0f%% "
            "power %d speed %.0f km/s | to point %.3e m, from star %.3e m, heading %.3f\n",
            world.components<ShipIdentity>().get(ship).name.c_str(), toString(pirates.values()[i].state),
            toString(control.mode), toString(control.phase), control.arrived ? 1 : 0, control.track,
            world.components<CombatControl>().get(ship).targetTrack, structureOf(modules)->fraction() * 100.0,
            hasPower(modules) ? 1 : 0, length(state.velocity) / 1000.0,
            length(control.point - state.position), length(state.position),
            dot(state.velocity / std::max(length(state.velocity), 1e-9),
                (control.point - state.position) / std::max(length(control.point - state.position), 1e-9)));
        // Debug truth: what the track really is.
        const SensorContact* contact = sandbox.sensors().findContact(content::kFactionPirates, control.track);
        if (contact != nullptr && world.isAlive(contact->target)) {
            const EntityId prey = contact->target;
            const ShipControl& preyControl = world.components<ShipControl>().get(prey);
            const ShipModules& preyModules = world.components<ShipModules>().get(prey);
            std::printf(
                "      prey %-12s mode %-8s arrived %d speed %.1f km/s structure %.0f%% power %d at %.0f km, "
                "level %s, lastSeen %.0f s ago\n",
                world.components<ShipIdentity>().get(prey).name.c_str(), toString(preyControl.mode),
                preyControl.arrived ? 1 : 0, length(world.components<Kinematics>().get(prey).velocity) / 1e3,
                structureOf(preyModules)->fraction() * 100.0, hasPower(preyModules) ? 1 : 0,
                length(world.components<Kinematics>().get(prey).position - state.position) / 1e3,
                toString(contact->level), (simulation.now() - contact->lastSeen).toSeconds());
        } else if (control.track != 0) {
            std::printf("      prey: track %u %s\n", control.track,
                        contact == nullptr ? "lost" : (contact->ghost ? "is a ghost" : "target gone"));
        }
    }
    printEconomy(simulation, sandbox, options.markets);
    if (options.dump) {
        const World& dumped = simulation.world();
        const auto nameOf = [&](EntityId entity) -> std::string {
            if (const CelestialBody* body = dumped.components<CelestialBody>().tryGet(entity)) {
                return body->name;
            }
            if (const ShipIdentity* ship = dumped.components<ShipIdentity>().tryGet(entity)) {
                return ship->name;
            }
            return "-";
        };
        const auto describe = [&](EntityId ship, const char* state) {
            const ShipControl& control = dumped.components<ShipControl>().get(ship);
            const ShipModules& modules = dumped.components<ShipModules>().get(ship);
            const Wallet* wallet = dumped.components<Wallet>().tryGet(ship);
            const CargoHold& hold = dumped.components<CargoHold>().get(ship);
            const TraderFinance* books = dumped.components<TraderFinance>().tryGet(ship);
            std::printf("  %-14s %-10s %-8s %-10s arrived %d target %-22s speed %8.1f km/s structure %3.0f%% "
                        "power %d "
                        "credits %7lld cargo %3u t",
                        nameOf(ship).c_str(), state, toString(control.mode), toString(control.phase),
                        control.arrived ? 1 : 0, nameOf(control.target).c_str(),
                        length(dumped.components<Kinematics>().get(ship).velocity) / 1e3,
                        structureOf(modules)->fraction() * 100.0, hasPower(modules) ? 1 : 0,
                        static_cast<long long>(wallet != nullptr ? wallet->credits : 0), hold.used());
            if (books != nullptr) {
                std::printf(" deposit %7lld debt %5lld", static_cast<long long>(books->deposit),
                            static_cast<long long>(books->debt));
            }
            std::printf("\n");
        };
        const ComponentStore<HaulerBrain>& haulerBrains = dumped.components<HaulerBrain>();
        for (usize i = 0; i < haulerBrains.size(); ++i) {
            const HaulerBrain& brain = haulerBrains.values()[i];
            const f64 wait = (brain.departAt - simulation.now()).toSeconds();
            describe(haulerBrains.entities()[i],
                     wait > 0.0 ? std::format("wait {:.0f}s", wait).c_str() : "ready");
        }
        const ComponentStore<PatrolBrain>& patrolBrains = dumped.components<PatrolBrain>();
        for (usize i = 0; i < patrolBrains.size(); ++i) {
            describe(patrolBrains.entities()[i], toString(patrolBrains.values()[i].state));
        }
    }
    std::printf("combat (attacker -> victim: hits / kills):\n");
    for (u32 a = 0; a <= kFactions; ++a) {
        for (u32 v = 0; v <= kFactions; ++v) {
            if (hits[a][v] > 0 || kills[a][v] > 0) {
                std::printf("  %-30s -> %-30s %8llu / %llu\n",
                            a < kFactions ? content::kFactionNames[a] : "?",
                            v < kFactions ? content::kFactionNames[v] : "?",
                            static_cast<unsigned long long>(hits[a][v]),
                            static_cast<unsigned long long>(kills[a][v]));
            }
        }
    }
    if (options.journal) {
        // Next to the executable (a package), else in the source tree (a build).
        Catalog catalog;
        const Catalog* language = nullptr;
        if (!options.language.empty() && options.language != "es") {
            for (const std::filesystem::path& dir :
                 {std::filesystem::path("data"), std::filesystem::path(GX_SOURCE_DATA_DIR)}) {
                std::ifstream in(dir / "lang" / (options.language + ".po"), std::ios::binary);
                std::string error;
                if (in && catalog.loadPo(std::string(std::istreambuf_iterator<char>(in), {}), error)) {
                    language = &catalog;
                    break;
                }
            }
            if (language == nullptr) {
                GX_LOG_WARN(kChannel, "no catalog for language '{}': the journal stays in Spanish",
                            options.language);
            }
        }
        for (const JournalEntry& entry : sandbox.journal()) {
            std::printf("  %s  %s\n", formatSimTime(entry.time, content::kEpochYear).c_str(),
                        render(entry.text, language).c_str());
        }
    }
    const SandboxStats& stats = sandbox.stats();
    std::printf("sandbox: %llu departures, %llu arrivals, %llu hunts, %llu haulers lost, %llu pirates lost, "
                "%llu pirates fled, %llu player deaths, final hash %016llx\n",
                static_cast<unsigned long long>(stats.haulerDepartures),
                static_cast<unsigned long long>(stats.arrivals), static_cast<unsigned long long>(stats.hunts),
                static_cast<unsigned long long>(stats.haulersLost),
                static_cast<unsigned long long>(stats.piratesLost),
                static_cast<unsigned long long>(stats.piratesLeft),
                static_cast<unsigned long long>(stats.playerDeaths),
                static_cast<unsigned long long>(simulation.stateHash()));
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    platform::setCurrentThreadName("main");
    Options options;
    if (const int exitCode = parseOptions(argc, argv, options); exitCode >= 0) {
        return exitCode;
    }
    logging::setLevel(options.logLevel);
    if (!options.inspectPath.empty()) {
        return inspectSave(options.inspectPath);
    }
    if (options.sandbox) {
        profiling::setEnabled(options.profile);
        return runSandbox(options);
    }

    Recording replay;
    if (!options.replayPath.empty()) {
        std::string error;
        if (!readRecording(options.replayPath, replay, error)) {
            GX_LOG_ERROR(kChannel, "cannot read replay {}: {}", options.replayPath, error);
            return 1;
        }
        options.seed = replay.seed;
        options.systems = replay.systems;
        options.bodies = replay.bodies;
        options.goods = replay.goods;
        options.days = replay.days;
        options.raids = false; // the recorded commands are the input
    }

    profiling::setEnabled(options.profile);
    if (!options.tracePath.empty()) {
        profiling::setTraceCapture(true, 4'000'000);
    }
    GX_LOG_INFO(
        kChannel,
        "GalaxyEngine {} ({}) headless | seed {} | {} star systems x {} bodies x {} goods | {} days | "
        "{} threads | {}",
        GX_VERSION, GX_BUILD_CONFIG, options.seed, options.systems, options.bodies, options.goods,
        options.days, options.threads,
        options.speed == 0 ? std::string("unpaced") : std::format("{}x", options.speed));

    SyntheticGalaxyConfig galaxyConfig;
    galaxyConfig.seed = options.seed;
    galaxyConfig.starSystems = options.systems;
    galaxyConfig.bodiesPerSystem = options.bodies;
    galaxyConfig.goodsPerSystem = options.goods;

    const Stopwatch generationTimer;
    SyntheticGalaxy galaxy(galaxyConfig);
    const f64 generationMs = generationTimer.elapsedMs();

    JobSystem jobs(options.threads - 1);
    Simulation simulation(Simulation::Config{.seed = options.seed}, jobs);
    galaxy.install(simulation);
    GX_LOG_INFO(kChannel, "generated {} bodies in {:.1f} ms | galaxy arrays {:.1f} MiB", galaxy.bodyCount(),
                generationMs, toMiB(galaxy.stateBytes()));

    if (!options.loadPath.empty()) {
        SaveFileContents save;
        std::string error;
        const Stopwatch loadTimer;
        if (!readSaveFile(options.loadPath, save, error) || !simulation.loadState(save.payload, error)) {
            GX_LOG_ERROR(kChannel, "cannot load {}: {}", options.loadPath, error);
            return 1;
        }
        GX_LOG_INFO(kChannel, "loaded {} ({:.1f} MiB) in {:.1f} ms: {} | {} convoys in flight",
                    options.loadPath, toMiB(save.payload.size()), loadTimer.elapsedMs(),
                    formatSimTime(simulation.now()), galaxy.activeConvoys());
    }

    const SimTime start = simulation.now();
    const SimTime end = start + SimDuration::days(options.days);
    simulation.commands().setRecording(!options.recordPath.empty());
    if (options.raids) {
        // Stand-in for player input: one raid per simulated day at noon, on a seeded pseudo-random system.
        Rng rng = Rng::forStream(options.seed, fnv1a64("headless.raids"),
                                 static_cast<u64>(start.microsecondsSinceEpoch()));
        for (u32 day = 0; day < options.days; ++day) {
            simulation.submitCommand(RaidConvoysCommand{rng.uniformU32(options.systems)},
                                     start + SimDuration::days(day) + SimDuration::hours(12));
        }
    }
    for (const CommandRecord& record : replay.commands) {
        simulation.commands().submitRecord(record);
    }

    const Stopwatch wall;
    if (options.speed == 0) {
        runUnpaced(simulation, galaxy, end, options.days, wall);
    } else {
        runPaced(simulation, galaxy, end, options.speed, wall);
    }
    const f64 wallSeconds = wall.elapsedSeconds();
    const u64 finalHash = simulation.stateHash();

    const JobSystemStats jobStats = jobs.stats();
    const SimulationStats& simStats = simulation.stats();
    const SyntheticGalaxyStats& galaxyStats = galaxy.stats();
    const ProcessMemory memory = queryProcessMemory();
    std::printf("\n== Headless run complete ==\n");
    std::printf("simulated          : %u days, now %s\n", options.days,
                formatSimTime(simulation.now()).c_str());
    std::printf("steps              : %llu (%llu system runs)\n",
                static_cast<unsigned long long>(simStats.steps),
                static_cast<unsigned long long>(simStats.systemRuns));
    std::printf("wall time          : %.3f s -> %.2f simulated days per second\n", wallSeconds,
                static_cast<f64>(options.days) / std::max(wallSeconds, 1e-9));
    std::printf("state hash         : %016llx\n", static_cast<unsigned long long>(finalHash));
    std::printf("entities           : %u alive (%u slots)\n", simulation.world().entityCount(),
                simulation.world().registry().slotCount());
    std::printf("convoys            : %llu spawned, %llu arrived, %llu raided, %zu in flight\n",
                static_cast<unsigned long long>(galaxyStats.convoysSpawned),
                static_cast<unsigned long long>(galaxyStats.convoysArrived),
                static_cast<unsigned long long>(galaxyStats.convoysRaided), galaxy.activeConvoys());
    std::printf("events / commands  : %llu events, %llu commands applied\n",
                static_cast<unsigned long long>(simStats.eventsEmitted),
                static_cast<unsigned long long>(simStats.commandsApplied));
    std::printf("galactic output    : %.6e\n", galaxy.galacticOutput());
    std::printf("jobs executed      : %llu (%llu parallelFor calls)\n",
                static_cast<unsigned long long>(jobStats.jobsExecuted),
                static_cast<unsigned long long>(jobStats.parallelForCalls));
    std::printf("memory             : working set %.1f MiB (peak %.1f MiB), private %.1f MiB\n",
                toMiB(memory.workingSetBytes), toMiB(memory.peakWorkingSetBytes), toMiB(memory.privateBytes));

    if (options.profile) {
        std::printf("\n%s", profiling::formatSummaryTable(profiling::summary()).c_str());
    }

    int exitCode = 0;
    if (!options.savePath.empty()) {
        const Stopwatch saveTimer;
        const std::vector<std::byte> payload = simulation.saveState();
        std::string error;
        const std::string description =
            std::format("synthetic galaxy {}x{}x{}, {}", options.systems, options.bodies, options.goods,
                        formatSimTime(simulation.now()));
        if (writeSaveFile(options.savePath, GX_VERSION, description, payload, error)) {
            GX_LOG_INFO(kChannel, "saved {} ({:.1f} MiB) in {:.1f} ms", options.savePath,
                        toMiB(payload.size()), saveTimer.elapsedMs());
        } else {
            GX_LOG_ERROR(kChannel, "cannot save {}: {}", options.savePath, error);
            exitCode = 1;
        }
    }
    if (!options.recordPath.empty()) {
        Recording recording{options.seed,  options.systems, options.bodies,
                            options.goods, options.days,    simulation.commands().recorded(),
                            finalHash};
        BinaryWriter writer;
        const auto mark = writer.beginChunk(kReplayChunk, 1);
        writer.io(recording);
        writer.endChunk(mark);
        std::string error;
        if (writeSaveFile(options.recordPath, GX_VERSION, "replay", writer.bytes(), error)) {
            GX_LOG_INFO(kChannel, "recorded {} commands and final hash {:016x} to {}",
                        recording.commands.size(), finalHash, options.recordPath);
        } else {
            GX_LOG_ERROR(kChannel, "cannot write recording {}: {}", options.recordPath, error);
            exitCode = 1;
        }
    }
    if (!options.replayPath.empty()) {
        if (finalHash == replay.finalHash) {
            std::printf("\nreplay            : MATCH (%zu commands, hash %016llx)\n", replay.commands.size(),
                        static_cast<unsigned long long>(finalHash));
        } else {
            std::printf("\nreplay            : MISMATCH (recorded %016llx, replayed %016llx)\n",
                        static_cast<unsigned long long>(replay.finalHash),
                        static_cast<unsigned long long>(finalHash));
            exitCode = 1;
        }
    }
    if (!options.tracePath.empty()) {
        if (profiling::writeChromeTrace(options.tracePath)) {
            GX_LOG_INFO(kChannel, "trace written to {} ({} events dropped)", options.tracePath,
                        profiling::droppedTraceEvents());
        } else {
            GX_LOG_ERROR(kChannel, "could not write trace to {}", options.tracePath);
            exitCode = 1;
        }
    }
    return exitCode;
}
