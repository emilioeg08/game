#pragma once

#include "Engine/Core/Assert.h"
#include "Simulation/Kernel/SystemScheduler.h"

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gx {

class EventChannelBase {
public:
    explicit EventChannelBase(std::string name) : m_name(std::move(name)) {}
    virtual ~EventChannelBase() = default;
    EventChannelBase(const EventChannelBase&) = delete;
    EventChannelBase& operator=(const EventChannelBase&) = delete;

    [[nodiscard]] const std::string& name() const { return m_name; }
    [[nodiscard]] virtual usize size() const = 0;
    // Runs the subscribers over every event not yet dispatched. Returns false if there was none.
    virtual bool dispatchPending(const TickContext& context) = 0;
    virtual void clear() = 0;

private:
    std::string m_name;
};

// Events of one type raised during the current step.
//
// Lifetime: emitted in any phase, dispatched to subscribers at the start of the EventResolution phase,
// readable through events() by later phases (History, PresentationSnapshot), discarded at the end of the
// step. Events are never saved: saves happen between steps.
//
// Determinism: serial code emits in program order. Parallel code emits into one buffer per parallelFor chunk
// (emitFromChunk); endParallel appends the buffers in chunk order, so the final order is independent of the
// thread count.
template <typename T>
class EventChannel final : public EventChannelBase {
public:
    using Handler = std::function<void(const T&, const TickContext&)>;

    using EventChannelBase::EventChannelBase;

    void emit(T event) {
        GX_ASSERT(!m_parallelActive, "emit() during a parallel section of '{}'; use emitFromChunk", name());
        m_events.push_back(std::move(event));
    }

    // Prepares one buffer per chunk: pass JobSystem::chunkCount(count, grain) of the parallelFor.
    void beginParallel(u32 chunkCount) {
        GX_CHECK(!m_parallelActive, "nested parallel emission into '{}'", name());
        if (m_chunks.size() < chunkCount) {
            m_chunks.resize(chunkCount);
        }
        m_activeChunks = chunkCount;
        m_parallelActive = true;
    }
    // Thread-safe as long as each chunk is emitted from by one thread at a time (true inside parallelFor).
    void emitFromChunk(u32 chunk, T event) {
        GX_ASSERT(m_parallelActive && chunk < m_activeChunks, "invalid chunk {} for '{}'", chunk, name());
        m_chunks[chunk].push_back(std::move(event));
    }
    void endParallel() {
        GX_CHECK(m_parallelActive, "endParallel() without beginParallel() on '{}'", name());
        for (u32 chunk = 0; chunk < m_activeChunks; ++chunk) {
            auto& buffer = m_chunks[chunk];
            m_events.insert(m_events.end(), std::make_move_iterator(buffer.begin()),
                            std::make_move_iterator(buffer.end()));
            buffer.clear(); // keeps capacity for the next step
        }
        m_parallelActive = false;
    }

    // Subscribers run on the main thread in subscription order and may emit further events.
    void subscribe(Handler handler) { m_handlers.push_back(std::move(handler)); }

    [[nodiscard]] std::span<const T> events() const { return m_events; }
    [[nodiscard]] usize size() const override { return m_events.size(); }

    bool dispatchPending(const TickContext& context) override {
        if (m_dispatched == m_events.size()) {
            return false;
        }
        // Only the events present now: events emitted by these handlers wait for the next pass, which is
        // what lets EventBus::dispatch detect a runaway cascade instead of looping forever here.
        const usize passEnd = m_events.size();
        while (m_dispatched < passEnd) {
            const T event = m_events[m_dispatched++]; // copy: handlers may emit and reallocate m_events
            for (const Handler& handler : m_handlers) {
                handler(event, context);
            }
        }
        return true;
    }

    void clear() override {
        GX_ASSERT(!m_parallelActive, "clearing '{}' during a parallel section", name());
        m_events.clear();
        m_dispatched = 0;
    }

private:
    std::vector<T> m_events;
    std::vector<std::vector<T>> m_chunks;
    std::vector<Handler> m_handlers;
    usize m_dispatched = 0;
    u32 m_activeChunks = 0;
    bool m_parallelActive = false;
};

// Registry of event channels, owned by the simulation kernel.
class EventBus {
public:
    // Dispatch passes before a cascade of events emitting events is considered runaway.
    static constexpr u32 kMaxDispatchPasses = 16;

    template <typename T>
    EventChannel<T>& registerEvent(std::string_view name) {
        GX_CHECK(!m_channelsByType.contains(typeKey<T>()), "event '{}' registered twice", name);
        auto channel = std::make_unique<EventChannel<T>>(std::string(name));
        EventChannel<T>& result = *channel;
        m_channelsByType.emplace(typeKey<T>(), channel.get());
        m_channels.push_back(std::move(channel));
        return result;
    }

    template <typename T>
    [[nodiscard]] EventChannel<T>& channel() {
        const auto it = m_channelsByType.find(typeKey<T>());
        GX_CHECK(it != m_channelsByType.end(), "event type not registered");
        return *static_cast<EventChannel<T>*>(it->second);
    }

    // Dispatches channels in registration order, repeating while handlers emit new events.
    void dispatch(const TickContext& context);
    // Discards this step's events and returns how many there were.
    usize clearAll();

    [[nodiscard]] usize channelCount() const { return m_channels.size(); }

private:
    template <typename T>
    static const void* typeKey() {
        static const char key = 0;
        return &key;
    }

    std::vector<std::unique_ptr<EventChannelBase>> m_channels;
    std::unordered_map<const void*, EventChannelBase*> m_channelsByType;
};

} // namespace gx
