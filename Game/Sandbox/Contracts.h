#pragma once

#include "Engine/Core/Types.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/Economy/Economy.h"
#include "Simulation/World/EntityRegistry.h"

#include <string>

// Contracts (ADR-031). Every contract comes from the simulation's real state: a delivery answers a real
// shortage at a port, a bounty names a pirate the Authority has actually identified. Deliveries are paid by
// the port itself (like its purchases); bounties by the Authority's treasury, which escrows the reward when
// posting (only from what its patrols do not need) and takes it back if nobody earns it.
namespace gx {

enum class ContractKind : u8 {
    Delivery, // take `tonnes` of `good` to `port`
    Bounty,   // destroy or capture `target`
    Count
};

enum class ContractState : u8 {
    Open,      // on the stations' board
    Accepted,  // taken by the player
    Completed, // reward paid
    Failed,    // accepted and not done in time (or abandoned): reputation penalty
    Expired,   // nobody took it in time
    Cancelled, // no longer possible through nobody's fault (the target died or left)
    Count
};

[[nodiscard]] const char* toString(ContractKind kind);
[[nodiscard]] const char* toString(ContractState state);

struct Contract {
    u32 id = 0;
    ContractKind kind = ContractKind::Delivery;
    ContractState state = ContractState::Open;
    EntityId port; // Delivery: destination. Bounty: the planet the target was last seen near
    GoodId good = 0;
    u32 tonnes = 0;
    u32 delivered = 0;
    EntityId target; // Bounty: the pirate (internal: the player only gets its name and last sighting)
    std::string targetName;
    i64 reward = 0;
    bool escrowed = false; // the reward was taken from the treasury when posted
    u32 holderFaction = 0; // who accepted it: the player, or a trader (Accepted and later)
    EntityId holder;       // the trader's ship (for the player: whichever ship it is flying)
    SimTime posted;
    SimTime deadline;
    SimTime closed; // when it left Open/Accepted

    [[nodiscard]] bool live() const {
        return state == ContractState::Open || state == ContractState::Accepted;
    }

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("id", id);
        ar.io("kind", kind);
        ar.io("state", state);
        ar.io("port", port);
        ar.io("good", good);
        ar.io("tonnes", tonnes);
        ar.io("delivered", delivered);
        ar.io("target", target);
        ar.io("targetName", targetName);
        ar.io("reward", reward);
        ar.io("escrowed", escrowed);
        ar.io("holderFaction", holderFaction);
        ar.io("holder", holder);
        ar.io("posted", posted);
        ar.io("deadline", deadline);
        ar.io("closed", closed);
    }
};

enum class ContractAction : u8 { Accept, Deliver, Abandon, Count };

// Player input on a contract. Validated by the handler.
struct ContractCommand {
    EntityId ship;
    u32 contract = 0;
    ContractAction action = ContractAction::Accept;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io("ship", ship);
        ar.io("contract", contract);
        ar.io("action", action);
    }
};

} // namespace gx
