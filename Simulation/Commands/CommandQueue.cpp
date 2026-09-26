#include "Simulation/Commands/CommandQueue.h"

#include "Engine/Core/Log.h"

#include <algorithm>
#include <format>
#include <tuple>

namespace gx {
namespace {
bool comesBefore(const CommandRecord& a, const CommandRecord& b) {
    return std::tie(a.executeAt, a.sequence) < std::tie(b.executeAt, b.sequence);
}
} // namespace

void CommandQueue::insert(CommandRecord record) {
    const auto position = std::upper_bound(m_pending.begin(), m_pending.end(), record, comesBefore);
    m_pending.insert(position, std::move(record));
}

void CommandQueue::submitRecord(CommandRecord record) {
    GX_CHECK(m_handlers.contains(record.type), "replayed command has an unknown type {:08x}", record.type);
    m_nextSequence = std::max(m_nextSequence, record.sequence + 1);
    insert(std::move(record));
}

SimTime CommandQueue::nextTime() const {
    GX_ASSERT(!m_pending.empty(), "no pending commands");
    return m_pending.front().executeAt;
}

u32 CommandQueue::applyDue(SimTime now, const TickContext& context) {
    const auto dueEnd = std::find_if(m_pending.begin(), m_pending.end(),
                                     [now](const CommandRecord& record) { return record.executeAt > now; });
    if (dueEnd == m_pending.begin()) {
        return 0;
    }
    // Detach the due commands first: handlers may submit new ones (which always land in the future).
    std::vector<CommandRecord> due(std::make_move_iterator(m_pending.begin()),
                                   std::make_move_iterator(dueEnd));
    m_pending.erase(m_pending.begin(), dueEnd);

    u32 applied = 0;
    for (CommandRecord& record : due) {
        const auto handler = m_handlers.find(record.type);
        if (handler == m_handlers.end()) {
            GX_LOG_WARN("Commands", "dropping command #{} of unknown type {:08x}", record.sequence,
                        record.type);
            continue;
        }
        BinaryReader payload(record.payload);
        handler->second.apply(payload, context);
        if (!payload.ok()) {
            GX_LOG_WARN("Commands", "dropping malformed command #{} '{}': {}", record.sequence,
                        handler->second.name, payload.error());
            continue;
        }
        ++applied;
        if (m_recording) {
            m_recorded.push_back(std::move(record));
        }
    }
    return applied;
}

void CommandQueue::write(BinaryWriter& writer) const {
    writer.io(m_nextSequence);
    writer.io(m_pending);
}

void CommandQueue::read(BinaryReader& reader) {
    u64 nextSequence = 0;
    std::vector<CommandRecord> pending;
    reader.io(nextSequence);
    reader.io(pending);
    if (!reader.ok()) {
        return;
    }
    for (usize i = 0; i < pending.size(); ++i) {
        if (!m_handlers.contains(pending[i].type)) {
            reader.fail(std::format("pending command has unknown type {:08x}", pending[i].type));
            return;
        }
        if (pending[i].sequence >= nextSequence || (i > 0 && !comesBefore(pending[i - 1], pending[i]))) {
            reader.fail("pending commands are out of order");
            return;
        }
    }
    m_nextSequence = nextSequence;
    m_pending = std::move(pending);
}

} // namespace gx
