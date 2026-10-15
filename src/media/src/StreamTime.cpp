#include <wabcut/media/StreamTime.h>

namespace wabcut::media {

std::optional<Ticks> streamToTicks(std::int64_t pts, TimeBase timeBase) {
    return rescale(pts, timeBase, Rounding::Nearest);
}

std::optional<std::int64_t> ticksToStream(Ticks t, TimeBase timeBase) {
    const auto unit = rescale(t, timeBase, Rounding::Floor);
    if (!unit) {
        return std::nullopt;
    }
    // streamToTicks() rounds to nearest, so the next unit's tick may lie (less than half a tick) below its
    // exact start; it is the answer when that rounded start is already <= t. This makes the round trip exact.
    if (const auto next = checkedAdd(*unit, 1)) {
        if (const auto nextStart = streamToTicks(*next, timeBase); nextStart && *nextStart <= t) {
            return *next;
        }
    }
    return unit;
}

} // namespace wabcut::media
