#pragma once

#include "Engine/Core/Assert.h"
#include "Engine/Core/Hash.h"
#include "Engine/Serialization/Binary.h"
#include "Engine/Time/SimTime.h"
#include "Simulation/Kernel/SystemScheduler.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gx {

using CommandTypeId = u32;

// A serialized command: what the player, a UI, a script or a network peer asks the simulation to do.
struct CommandRecord {
    SimTime executeAt;
    u64 sequence = 0; // submission order: tie-break for commands at the same instant
    CommandTypeId type = 0;
    std::vector<std::byte> payload;

    template <typename Archive>
    void io(Archive& ar) {
        ar.io(executeAt);
        ar.io(sequence);
        ar.io(type);
        ar.io(payload);
    }
};

// Time-stamped external input. Commands are data (serialized at submission), applied by the kernel at the
// start of the Commands phase of the step at `executeAt`, in (executeAt, sequence) order. Because commands
// only ever target the future, a recorded log replayed on a fresh simulation produces the same state.
//
// Handlers receive untrusted input: they must validate it and ignore invalid commands, never assert.
class CommandQueue {
public:
    // Type ids derive from the name, so they are stable across builds and registration orders.
    template <typename C, typename Fn>
    void registerCommand(std::string_view name, Fn&& handler) {
        const auto type = static_cast<CommandTypeId>(fnv1a64(name));
        GX_CHECK(!m_handlers.contains(type), "command '{}' registered twice or its id collides", name);
        m_typeIds.emplace(typeKey<C>(), type);
        m_handlers.emplace(
            type, Entry{std::string(name),
                        [fn = std::forward<Fn>(handler)](BinaryReader& payload, const TickContext& context) {
                            C command{};
                            payload.io(command);
                            if (payload.ok()) {
                                fn(command, context);
                            }
                        }});
    }

    // `executeAt` must be in the future (the kernel clamps it). Returns the sequence number.
    template <typename C>
    u64 submit(const C& command, SimTime executeAt) {
        const auto it = m_typeIds.find(typeKey<C>());
        GX_CHECK(it != m_typeIds.end(), "command type not registered");
        BinaryWriter writer;
        writer.io(command);
        CommandRecord record{executeAt, m_nextSequence++, it->second, writer.takeBytes()};
        const u64 sequence = record.sequence;
        insert(std::move(record));
        return sequence;
    }

    // Re-submits a recorded command (replay), keeping its original time and sequence.
    void submitRecord(CommandRecord record);

    [[nodiscard]] bool hasPending() const { return !m_pending.empty(); }
    [[nodiscard]] SimTime nextTime() const;
    [[nodiscard]] usize pendingCount() const { return m_pending.size(); }

    // Applies every command due at or before `now`. Returns how many were applied.
    u32 applyDue(SimTime now, const TickContext& context);

    // When recording, every applied command is appended to recorded() (a replay log).
    void setRecording(bool recording) { m_recording = recording; }
    [[nodiscard]] const std::vector<CommandRecord>& recorded() const { return m_recorded; }

    // Pending commands are part of the simulation state and are saved.
    void write(BinaryWriter& writer) const;
    void read(BinaryReader& reader);

private:
    struct Entry {
        std::string name;
        std::function<void(BinaryReader&, const TickContext&)> apply;
    };

    template <typename T>
    static const void* typeKey() {
        static const char key = 0;
        return &key;
    }
    void insert(CommandRecord record);

    std::unordered_map<CommandTypeId, Entry> m_handlers;
    std::unordered_map<const void*, CommandTypeId> m_typeIds;
    std::vector<CommandRecord> m_pending; // sorted by (executeAt, sequence)
    u64 m_nextSequence = 1;
    bool m_recording = false;
    std::vector<CommandRecord> m_recorded;
};

} // namespace gx
