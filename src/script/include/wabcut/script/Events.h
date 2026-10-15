#pragma once

// What happens in the editor that extensions can follow (wabcut.events.on in scripts), and the throttle that keeps a
// scrubbing playhead from flooding them: each kind is passed on at most once per interval, the latest value winning;
// `coalesced` says how many were folded into it.

#include <wabcut/script/Manifest.h>

#include <array>
#include <chrono>
#include <optional>
#include <string_view>
#include <vector>

namespace wabcut::script {

enum class EventKind {
    SelectionChanged, ///< {"sequence": id, "clips": [ids], "markers": [ids]}
    PlayheadMoved,    ///< {"sequence": id, "time": ticks, "timecode": "00:00:01:00"}
    ProjectSaved,     ///< {"file": "...wabcut"}
    CommandApplied,   ///< {"label": "Add Marker", "kind": "edit" | "undo" | "redo", "revision": n}
};
inline constexpr std::size_t kEventKinds = 4;

std::string_view toString(EventKind kind); ///< "selectionChanged", "playheadMoved", ...
std::optional<EventKind> eventKindFromString(std::string_view text);

struct Event {
    EventKind kind = EventKind::CommandApplied;
    Json data = Json::object();
    int coalesced = 0; ///< earlier events of this kind it replaces
};

class EventThrottle {
public:
    using Clock = std::chrono::steady_clock;
    /// The default intervals: 100 ms for selection, playhead and applied commands (10 per second), 250 ms for saves.
    EventThrottle();
    void setInterval(EventKind kind, std::chrono::milliseconds interval);

    /// The event to pass on now, or nullopt when it has to wait (it is then held, replacing a held one).
    std::optional<Event> offer(Event event, Clock::time_point now);
    /// The held events whose interval has passed.
    std::vector<Event> due(Clock::time_point now);
    /// When the next held event becomes due.
    std::optional<Clock::time_point> nextDue() const;

private:
    struct Slot {
        std::chrono::milliseconds interval{100};
        std::optional<Clock::time_point> lastSent;
        std::optional<Event> held;
    };
    std::array<Slot, kEventKinds> m_slots;
};

} // namespace wabcut::script
