#include <wabcut/script/Events.h>

namespace wabcut::script {

namespace {

constexpr std::array<std::string_view, kEventKinds> kNames{"selectionChanged", "playheadMoved", "projectSaved",
                                                           "commandApplied"};

} // namespace

std::string_view toString(EventKind kind) { return kNames[static_cast<std::size_t>(kind)]; }

std::optional<EventKind> eventKindFromString(std::string_view text) {
    for (std::size_t i = 0; i < kNames.size(); ++i) {
        if (kNames[i] == text) {
            return static_cast<EventKind>(i);
        }
    }
    return std::nullopt;
}

EventThrottle::EventThrottle() {
    setInterval(EventKind::ProjectSaved, std::chrono::milliseconds(250));
}

void EventThrottle::setInterval(EventKind kind, std::chrono::milliseconds interval) {
    m_slots[static_cast<std::size_t>(kind)].interval = interval;
}

std::optional<Event> EventThrottle::offer(Event event, Clock::time_point now) {
    Slot& slot = m_slots[static_cast<std::size_t>(event.kind)];
    if (slot.held) {
        event.coalesced += slot.held->coalesced + 1;
        slot.held = std::move(event);
        return std::nullopt;
    }
    if (slot.lastSent && now - *slot.lastSent < slot.interval) {
        slot.held = std::move(event);
        return std::nullopt;
    }
    slot.lastSent = now;
    return event;
}

std::vector<Event> EventThrottle::due(Clock::time_point now) {
    std::vector<Event> out;
    for (Slot& slot : m_slots) {
        if (slot.held && (!slot.lastSent || now - *slot.lastSent >= slot.interval)) {
            slot.lastSent = now;
            out.push_back(std::move(*slot.held));
            slot.held.reset();
        }
    }
    return out;
}

std::optional<EventThrottle::Clock::time_point> EventThrottle::nextDue() const {
    std::optional<Clock::time_point> next;
    for (const Slot& slot : m_slots) {
        if (slot.held) {
            const Clock::time_point at = slot.lastSent ? *slot.lastSent + slot.interval : Clock::time_point{};
            next = next ? std::min(*next, at) : at;
        }
    }
    return next;
}

} // namespace wabcut::script
