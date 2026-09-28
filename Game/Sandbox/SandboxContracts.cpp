// Contracts of the Sandbox (ADR-031): posted from real shortages (paid by the port) and real identified
// pirates (paid by the Authority's treasury: escrowed when posted, refunded when nobody earns them).
#include "Game/Sandbox/Content.h"
#include "Game/Sandbox/Sandbox.h"

#include "Engine/Core/Log.h"
#include "Engine/Text/Localization.h"
#include "Simulation/Kernel/Simulation.h"
#include "Space/Bodies/CelestialBody.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace gx {

const char* toString(ContractKind kind) {
    switch (kind) {
    case ContractKind::Delivery:
        return "Delivery";
    case ContractKind::Bounty:
        return "Bounty";
    case ContractKind::Count:
        break;
    }
    return "?";
}

const char* toString(ContractState state) {
    switch (state) {
    case ContractState::Open:
        return "Open";
    case ContractState::Accepted:
        return "Accepted";
    case ContractState::Completed:
        return "Completed";
    case ContractState::Failed:
        return "Failed";
    case ContractState::Expired:
        return "Expired";
    case ContractState::Cancelled:
        return "Cancelled";
    case ContractState::Count:
        break;
    }
    return "?";
}

Message Sandbox::describe(const Contract& contract) const {
    const World& world = m_simulation->world();
    if (contract.kind == ContractKind::Delivery) {
        return msg("{} t de {} a {}", number(contract.tonnes), term(m_economy.goods()[contract.good].name),
                   named(world, contract.port));
    }
    return msg("abatir o capturar al pirata {} (visto cerca de {})", literal(contract.targetName),
               named(world, contract.port));
}

void Sandbox::closeContract(Contract& contract, ContractState state, SimTime now) {
    const bool wasAccepted = contract.state == ContractState::Accepted;
    const bool player = wasAccepted && contract.holderFaction == content::kFactionPlayer;
    contract.state = state;
    contract.closed = now;
    World& world = m_simulation->world();
    switch (state) {
    case ContractState::Completed:
        ++m_stats.contractsCompleted;
        m_stats.contractRewardsPaid += contract.reward;
        if (!player) {
            // A trader earned it (ADR-032): the reward goes to its purse, the news to everybody.
            if (Wallet* wallet = world.isAlive(contract.holder)
                                     ? world.components<Wallet>().tryGet(contract.holder)
                                     : nullptr) {
                wallet->credits += contract.reward;
            }
            ++m_stats.contractsCompletedByTraders;
            addJournal(now,
                       msg("Noticias: {} cumple un contrato de suministro ({}).",
                           named(world, contract.holder), describe(contract)),
                       JournalKind::News);
            return;
        }
        m_company.account.credits += contract.reward;
        m_company.totals.contracts += contract.reward;
        changeReputation(content::kReputationContractDone);
        addJournal(now,
                   msg("Contrato cumplido: {}. Cobras {} cr.", describe(contract), number(contract.reward)));
        return; // the escrow (or the port's purse) is the reward
    case ContractState::Failed:
        ++m_stats.contractsFailed;
        if (player) {
            changeReputation(content::kReputationContractFailed);
            addJournal(now, msg("Contrato fallido: {}. Reputación {}.", describe(contract),
                                number(m_reputation, 0)));
        }
        break;
    case ContractState::Expired:
        ++m_stats.contractsExpired;
        break;
    case ContractState::Cancelled:
        ++m_stats.contractsCancelled;
        if (player) {
            addJournal(now, msg("Contrato cancelado: {}.", describe(contract)));
        }
        break;
    default:
        break;
    }
    if (contract.escrowed) {
        m_treasury += contract.reward; // nobody earned it: back to the treasury
    }
}

void Sandbox::deliverTraderContracts(World& world, EntityId ship, EntityId port, SimTime now) {
    CargoHold* hold = world.components<CargoHold>().tryGet(ship);
    Market* market = world.components<Market>().tryGet(port);
    if (hold == nullptr || market == nullptr) {
        return;
    }
    for (Contract& contract : m_contracts) {
        if (contract.state != ContractState::Accepted || contract.holderFaction == content::kFactionPlayer ||
            contract.holder != ship || contract.port != port) {
            continue;
        }
        MarketGood* good = market->find(contract.good);
        if (good == nullptr) {
            continue;
        }
        // Straight into the port's stock, like the player's deliveries.
        const u32 tonnes = hold->remove(contract.good, contract.tonnes - contract.delivered);
        good->stock += tonnes;
        good->boughtFromShips += tonnes;
        contract.delivered += tonnes;
        m_stats.tonnesDelivered += tonnes;
        if (contract.delivered >= contract.tonnes) {
            closeContract(contract, ContractState::Completed, now);
        }
    }
}

void Sandbox::settleBounty(EntityId pirate, bool byPlayer, SimTime now) {
    for (Contract& contract : m_contracts) {
        if (contract.kind == ContractKind::Bounty && contract.live() && contract.target == pirate) {
            closeContract(contract,
                          byPlayer && contract.state == ContractState::Accepted ? ContractState::Completed
                                                                                : ContractState::Cancelled,
                          now);
        }
    }
}

void Sandbox::updateContracts(const TickContext& context) {
    World& world = context.world;
    const SimTime now = context.now;

    // Deadlines, and bounties whose target is gone (killed by someone else, or left the system).
    for (Contract& contract : m_contracts) {
        if (!contract.live()) {
            continue;
        }
        if (contract.kind == ContractKind::Bounty && !world.isAlive(contract.target)) {
            closeContract(contract, ContractState::Cancelled, now);
        } else if (contract.state == ContractState::Accepted &&
                   contract.holderFaction != content::kFactionPlayer && !world.isAlive(contract.holder) &&
                   now < contract.deadline) {
            contract.state = ContractState::Open; // its trader was lost or went bankrupt: still needed
            contract.holder = {};
        } else if (now >= contract.deadline) {
            closeContract(contract,
                          contract.state == ContractState::Accepted ? ContractState::Failed
                                                                    : ContractState::Expired,
                          now);
        }
    }
    std::erase_if(m_contracts, [&](const Contract& contract) {
        return !contract.live() && now - contract.closed > content::kContractHistory;
    });
    usize open =
        static_cast<usize>(std::count_if(m_contracts.begin(), m_contracts.end(),
                                         [](const Contract& c) { return c.state == ContractState::Open; }));
    // Patrols come first: contracts only use what is left after the fleet's next purchase and its upkeep
    // reserve.
    const usize fleet = world.components<PatrolBrain>().size();
    const usize planned = fleet + (fleet < m_config.maxPatrols ? 1 : 0);
    const i64 patrolBudget = (fleet < m_config.maxPatrols ? content::kPatrolCommissionCost : 0) +
                             std::llround(static_cast<f64>(content::kPatrolUpkeepPerMinute) * 60.0 *
                                          content::kPatrolReserveHours * static_cast<f64>(planned));
    const auto affordable = [&](i64 reward) {
        return m_treasury - reward >= content::kContractTreasuryReserve + patrolBudget;
    };
    const auto post = [&](Contract contract) {
        contract.id = m_nextContractId++;
        contract.state = ContractState::Open;
        contract.posted = now;
        if (contract.escrowed) {
            m_treasury -= contract.reward;
        }
        ++m_stats.contractsPosted;
        m_contracts.push_back(std::move(contract));
        ++open;
    };

    // Real shortages: a port short of something its people or industry use asks for a delivery.
    const ComponentStore<Market>& markets = world.components<Market>();
    for (const EntityId port : m_ports) {
        const Market* market = markets.tryGet(port);
        for (usize g = 0; market != nullptr && g < market->goods.size() && open < content::kMaxOpenContracts;
             ++g) {
            const MarketGood& good = market->goods[g];
            const bool scarce = good.target > 0.0 &&
                                good.stock / good.target < content::kContractShortageRatio &&
                                consumptionRate(*market, good.good) > 0.0;
            const bool posted = std::any_of(m_contracts.begin(), m_contracts.end(), [&](const Contract& c) {
                return c.live() && c.kind == ContractKind::Delivery && c.port == port && c.good == good.good;
            });
            Contract contract;
            contract.kind = ContractKind::Delivery;
            contract.port = port;
            contract.good = good.good;
            contract.tonnes = content::kContractTonnes;
            contract.reward = std::llround(static_cast<f64>(contract.tonnes) *
                                           m_economy.basePrice(good.good) * content::kContractPremium);
            contract.deadline = now + content::kContractDeliveryTime;
            if (scarce && !posted) { // the port pays: no escrow
                post(contract);
                addJournal(now,
                           msg("Noticias: escasez de {} en {}. Contrato de suministro: {} cr.",
                               term(m_economy.goods()[good.good].name), named(world, port),
                               number(contract.reward)),
                           JournalKind::News);
                break; // one request per port at a time
            }
        }
    }

    // Identified pirates (by the Authority's patrols or reported by the traders): wanted, by name.
    for (const u32 faction : {content::kFactionAuthority, content::kFactionIndependent}) {
        for (const SensorContact& contact : m_sensors.picture(faction).contacts) {
            if (open >= content::kMaxOpenContracts || contact.ghost ||
                contact.level != ContactLevel::Identified || contact.faction != content::kFactionPirates ||
                !world.isAlive(contact.target) || !affordable(content::kContractBountyReward)) {
                continue;
            }
            const bool posted = std::any_of(m_contracts.begin(), m_contracts.end(), [&](const Contract& c) {
                return c.live() && c.kind == ContractKind::Bounty && c.target == contact.target;
            });
            if (posted) {
                continue;
            }
            Contract contract;
            contract.kind = ContractKind::Bounty;
            contract.target = contact.target;
            contract.targetName = world.components<ShipIdentity>().get(contact.target).name;
            contract.reward = content::kContractBountyReward;
            contract.escrowed = true;
            contract.deadline = now + content::kContractBountyTime;
            f64 nearest = 0.0;
            for (const EntityId planet : m_planets) {
                const f64 distance = length(bodyStateAt(world, planet, now).position - contact.position);
                if (!contract.port.isValid() || distance < nearest) {
                    contract.port = planet;
                    nearest = distance;
                }
            }
            post(contract);
            addJournal(now,
                       msg("Noticias: se busca al pirata {}, visto cerca de {}. Recompensa: {} cr.",
                           literal(contract.targetName), named(world, contract.port),
                           number(contract.reward)),
                       JournalKind::News);
        }
    }
}

void Sandbox::onContractCommand(const ContractCommand& command, const TickContext& context) {
    World& world = context.world;
    const auto reject = [&](const char* reason, const char* message) {
        ++m_stats.commandsRejected;
        GX_LOG_WARN("Sandbox", "contract command rejected: {}", reason);
        addJournal(context.now, msg(message));
    };
    const ShipIdentity* identity =
        world.isAlive(command.ship) ? world.components<ShipIdentity>().tryGet(command.ship) : nullptr;
    const auto it = std::find_if(m_contracts.begin(), m_contracts.end(),
                                 [&](const Contract& c) { return c.id == command.contract; });
    if (identity == nullptr || identity->faction != content::kFactionPlayer || it == m_contracts.end()) {
        reject("no such contract or ship", GX_TEXT("Ese contrato ya no existe."));
        return;
    }
    Contract& contract = *it;
    const EntityId docked = dockedPort(world, command.ship);
    const CelestialBody* dockedBody = world.components<CelestialBody>().tryGet(docked);
    switch (command.action) {
    case ContractAction::Accept: {
        const usize accepted =
            static_cast<usize>(std::count_if(m_contracts.begin(), m_contracts.end(), [](const Contract& c) {
                return c.state == ContractState::Accepted;
            }));
        if (contract.state != ContractState::Open) {
            reject("not open", GX_TEXT("Ese contrato ya no está disponible."));
        } else if (dockedBody == nullptr || dockedBody->kind != BodyKind::Station) {
            reject("not at a station", GX_TEXT("Los contratos se aceptan en el tablón de una estación."));
        } else if (hostile()) {
            reject("hostile", GX_TEXT("La Autoridad no da contratos a quien considera hostil."));
        } else if (accepted >= content::kMaxAcceptedContracts) {
            reject("too many", GX_TEXT("Ya tienes tres contratos en curso."));
        } else {
            contract.state = ContractState::Accepted;
            contract.holderFaction = content::kFactionPlayer;
            contract.holder = command.ship;
            addJournal(context.now,
                       msg("Contrato aceptado: {} ({} cr).", describe(contract), number(contract.reward)));
        }
        return;
    }
    case ContractAction::Deliver: {
        CargoHold* hold = world.components<CargoHold>().tryGet(command.ship);
        Market* market = world.components<Market>().tryGet(docked);
        MarketGood* good = market != nullptr ? market->find(contract.good) : nullptr;
        if (contract.state != ContractState::Accepted || contract.kind != ContractKind::Delivery ||
            contract.holderFaction != content::kFactionPlayer) {
            reject("nothing to deliver", GX_TEXT("Ese contrato no es de entrega o no es tuyo."));
        } else if (docked != contract.port || good == nullptr || hold == nullptr) {
            reject("wrong port", GX_TEXT("Tienes que estar atracado en el puerto de destino."));
        } else if (hold->amount(contract.good) == 0) {
            reject("no cargo", GX_TEXT("No llevas ese bien en la bodega."));
        } else {
            // Straight into the port's stock (no sale: the contract pays instead).
            const u32 tonnes = hold->remove(contract.good, contract.tonnes - contract.delivered);
            good->stock += tonnes;
            good->boughtFromShips += tonnes;
            contract.delivered += tonnes;
            if (contract.delivered >= contract.tonnes) {
                closeContract(contract, ContractState::Completed, context.now);
            } else {
                addJournal(context.now, msg("Entregas {} t: faltan {} t.", number(tonnes),
                                            number(contract.tonnes - contract.delivered)));
            }
        }
        return;
    }
    case ContractAction::Abandon:
        if (contract.state != ContractState::Accepted || contract.holderFaction != content::kFactionPlayer) {
            reject("not yours", GX_TEXT("Ese contrato no es tuyo."));
        } else {
            closeContract(contract, ContractState::Failed, context.now);
        }
        return;
    case ContractAction::Count:
        break;
    }
    reject("invalid action", GX_TEXT("Acción de contrato no válida."));
}

} // namespace gx
