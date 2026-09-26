#include "Simulation/Events/EventBus.h"

namespace gx {

void EventBus::dispatch(const TickContext& context) {
    for (u32 pass = 0;; ++pass) {
        GX_CHECK(pass < kMaxDispatchPasses, "event cascade did not settle after {} passes",
                 kMaxDispatchPasses);
        if (pass >= kMaxDispatchPasses) {
            return;
        }
        bool dispatchedAny = false;
        for (const auto& channel : m_channels) {
            dispatchedAny = channel->dispatchPending(context) || dispatchedAny;
        }
        if (!dispatchedAny) {
            return;
        }
    }
}

usize EventBus::clearAll() {
    usize total = 0;
    for (const auto& channel : m_channels) {
        total += channel->size();
        channel->clear();
    }
    return total;
}

} // namespace gx
